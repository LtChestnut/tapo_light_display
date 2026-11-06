#include "tapo_klap.h"

#include <WiFiClient.h>
#include <esp_random.h>
#include <mbedtls/aes.h>
#include <mbedtls/sha1.h>
#include <mbedtls/sha256.h>
#include <string.h>
#include <time.h>

namespace tapo {
namespace {

constexpr uint16_t kPort = 80;
constexpr uint32_t kConnectTimeoutMs = 3000;
constexpr uint32_t kIoTimeoutMs = 4000;
constexpr size_t kMaxResponse = 16 * 1024;
constexpr uint32_t kSessionBufferS = 20 * 60;  // re-handshake 20 min before device expiry

struct Sha256 {
  mbedtls_sha256_context ctx;
  Sha256() {
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
  }
  ~Sha256() { mbedtls_sha256_free(&ctx); }
  Sha256 &add(const void *p, size_t n) {
    mbedtls_sha256_update(&ctx, static_cast<const uint8_t *>(p), n);
    return *this;
  }
  void finish(uint8_t out[32]) { mbedtls_sha256_finish(&ctx, out); }
};

void be32(int32_t v, uint8_t out[4]) {
  const uint32_t u = static_cast<uint32_t>(v);
  out[0] = u >> 24;
  out[1] = u >> 16;
  out[2] = u >> 8;
  out[3] = u;
}

bool readLine(WiFiClient &c, String &line, uint32_t deadline) {
  line = "";
  while (millis() < deadline) {
    while (c.available()) {
      const char ch = static_cast<char>(c.read());
      if (ch == '\n') {
        if (line.endsWith("\r")) line.remove(line.length() - 1);
        return true;
      }
      line += ch;
      if (line.length() > 2048) return false;
    }
    if (!c.connected()) return false;
    delay(1);
  }
  return false;
}

}  // namespace

void computeAuthHash(const char *email, const char *password, uint8_t out[32]) {
  uint8_t h[40];
  mbedtls_sha1(reinterpret_cast<const uint8_t *>(email), strlen(email), h);
  mbedtls_sha1(reinterpret_cast<const uint8_t *>(password), strlen(password), h + 20);
  Sha256().add(h, sizeof(h)).finish(out);
}

void handshake1Hash(const uint8_t local[16], const uint8_t remote[16], const uint8_t auth[32], uint8_t out[32]) {
  Sha256().add(local, 16).add(remote, 16).add(auth, 32).finish(out);
}

void handshake2Hash(const uint8_t local[16], const uint8_t remote[16], const uint8_t auth[32], uint8_t out[32]) {
  Sha256().add(remote, 16).add(local, 16).add(auth, 32).finish(out);
}

// ---------------------------------------------------------------------------
// KlapCipher

void KlapCipher::init(const uint8_t ls[16], const uint8_t rs[16], const uint8_t ah[32]) {
  uint8_t d[32];
  Sha256().add("lsk", 3).add(ls, 16).add(rs, 16).add(ah, 32).finish(d);
  memcpy(key_, d, 16);
  Sha256().add("iv", 2).add(ls, 16).add(rs, 16).add(ah, 32).finish(d);
  memcpy(iv_, d, 12);
  seq_ = static_cast<int32_t>((uint32_t(d[28]) << 24) | (uint32_t(d[29]) << 16) |
                              (uint32_t(d[30]) << 8) | uint32_t(d[31]));
  Sha256().add("ldk", 3).add(ls, 16).add(rs, 16).add(ah, 32).finish(d);
  memcpy(sig_, d, 28);
}

int32_t KlapCipher::encrypt(const uint8_t *plain, size_t len, std::vector<uint8_t> &out) {
  seq_ = static_cast<int32_t>(static_cast<uint32_t>(seq_) + 1);  // wraps like the device

  const size_t pad = 16 - (len % 16);
  const size_t ctLen = len + pad;
  out.assign(32 + ctLen, 0);
  uint8_t *ct = out.data() + 32;
  memcpy(ct, plain, len);
  memset(ct + len, static_cast<int>(pad), pad);

  uint8_t iv[16];
  memcpy(iv, iv_, 12);
  be32(seq_, iv + 12);

  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  mbedtls_aes_setkey_enc(&aes, key_, 128);
  mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_ENCRYPT, ctLen, iv, ct, ct);
  mbedtls_aes_free(&aes);

  uint8_t seqBytes[4];
  be32(seq_, seqBytes);
  Sha256().add(sig_, 28).add(seqBytes, 4).add(ct, ctLen).finish(out.data());
  return seq_;
}

bool KlapCipher::decrypt(const uint8_t *data, size_t len, std::string &out) const {
  if (len < 32 + 16 || (len - 32) % 16 != 0) return false;
  const size_t ctLen = len - 32;
  std::vector<uint8_t> pt(ctLen);

  uint8_t iv[16];
  memcpy(iv, iv_, 12);
  be32(seq_, iv + 12);

  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  mbedtls_aes_setkey_dec(&aes, key_, 128);
  mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_DECRYPT, ctLen, iv, data + 32, pt.data());
  mbedtls_aes_free(&aes);

  const uint8_t pad = pt[ctLen - 1];
  if (pad == 0 || pad > 16) return false;
  for (size_t i = ctLen - pad; i < ctLen; ++i)
    if (pt[i] != pad) return false;
  out.assign(reinterpret_cast<const char *>(pt.data()), ctLen - pad);
  return true;
}

void KlapCipher::exportTo(KlapSession &s) const {
  memcpy(s.key, key_, 16);
  memcpy(s.iv, iv_, 12);
  memcpy(s.sig, sig_, 28);
  s.seq = seq_;
}

void KlapCipher::importFrom(const KlapSession &s) {
  memcpy(key_, s.key, 16);
  memcpy(iv_, s.iv, 12);
  memcpy(sig_, s.sig, 28);
  seq_ = s.seq;
}

// ---------------------------------------------------------------------------
// KlapClient

void KlapClient::exportSession(KlapSession &s) const {
  memset(&s, 0, sizeof(s));
  const int32_t remainingMs = static_cast<int32_t>(sessionExpiresMs_ - millis());
  if (!sessionValid_ || remainingMs <= 0 || cookie_.length() >= sizeof(s.cookie)) return;
  cipher_.exportTo(s);
  strlcpy(s.cookie, cookie_.c_str(), sizeof(s.cookie));
  s.expiresAtS = static_cast<int64_t>(time(nullptr)) + remainingMs / 1000;
  s.valid = true;
}

void KlapClient::importSession(const KlapSession &s) {
  sessionValid_ = false;
  const int64_t remainingS = s.expiresAtS - static_cast<int64_t>(time(nullptr));
  if (!s.valid || remainingS <= 0) return;
  cipher_.importFrom(s);
  cookie_ = s.cookie;
  sessionExpiresMs_ = millis() + static_cast<uint32_t>(remainingS) * 1000UL;
  sessionValid_ = true;
}

void KlapClient::begin(const char *host, const uint8_t authHash[32]) {
  host_ = host;
  memcpy(authHash_, authHash, 32);
  sessionValid_ = false;
}

bool KlapClient::post(const char *path, const uint8_t *body, size_t len, int &status,
                      std::vector<uint8_t> &resp, bool captureCookie) {
  status = 0;
  resp.clear();
  WiFiClient c;
  if (!c.connect(host_.c_str(), kPort, kConnectTimeoutMs)) {
    err_ = "connect failed";
    return false;
  }
  c.setNoDelay(true);

  String hdr;
  hdr.reserve(256);
  hdr += "POST ";
  hdr += path;
  hdr += " HTTP/1.1\r\nHost: ";
  hdr += host_;
  hdr += "\r\nAccept: */*\r\nContent-Type: application/octet-stream\r\nContent-Length: ";
  hdr += String(static_cast<unsigned>(len));
  if (!captureCookie && cookie_.length()) {
    hdr += "\r\nCookie: ";
    hdr += cookie_;
  }
  hdr += "\r\nConnection: close\r\n\r\n";
  c.write(reinterpret_cast<const uint8_t *>(hdr.c_str()), hdr.length());
  if (len) c.write(body, len);

  const uint32_t deadline = millis() + kIoTimeoutMs;
  String line;
  if (!readLine(c, line, deadline) || !line.startsWith("HTTP/1.")) {
    err_ = "no HTTP status";
    c.stop();
    return false;
  }
  status = line.substring(9, 12).toInt();

  long contentLength = -1;
  while (readLine(c, line, deadline) && line.length()) {
    String lower = line;
    lower.toLowerCase();
    if (lower.startsWith("content-length:")) {
      contentLength = line.substring(15).toInt();
    } else if (captureCookie && lower.startsWith("set-cookie:")) {
      // e.g. "Set-Cookie: TP_SESSIONID=ABCDEF;TIMEOUT=86400"
      const int sid = line.indexOf("TP_SESSIONID=");
      if (sid >= 0) {
        int end = line.indexOf(';', sid);
        cookie_ = end > 0 ? line.substring(sid, end) : line.substring(sid);
        cookie_.trim();
      }
      const int to = line.indexOf("TIMEOUT=");
      if (to >= 0) cookieTimeoutS_ = line.substring(to + 8).toInt();
    }
  }

  if (contentLength > static_cast<long>(kMaxResponse)) {
    err_ = "response too large";
    c.stop();
    return false;
  }
  if (contentLength >= 0) resp.reserve(contentLength);
  while (millis() < deadline && (contentLength < 0 || resp.size() < static_cast<size_t>(contentLength))) {
    const int n = c.available();
    if (n > 0) {
      uint8_t buf[512];
      const int r = c.read(buf, n > static_cast<int>(sizeof(buf)) ? sizeof(buf) : n);
      if (r > 0) resp.insert(resp.end(), buf, buf + r);
      if (resp.size() > kMaxResponse) break;
    } else if (!c.connected()) {
      break;
    } else {
      delay(1);
    }
  }
  c.stop();
  if (contentLength >= 0 && resp.size() != static_cast<size_t>(contentLength)) {
    err_ = "short read";
    return false;
  }
  return true;
}

bool KlapClient::handshake() {
  sessionValid_ = false;
  cookie_ = "";
  cookieTimeoutS_ = 0;

  uint8_t localSeed[16];
  esp_fill_random(localSeed, sizeof(localSeed));

  int status = 0;
  std::vector<uint8_t> resp;
  if (!post("/app/handshake1", localSeed, 16, status, resp, true)) return false;
  if (status != 200 || resp.size() < 48) {
    err_ = "handshake1 HTTP " + std::to_string(status);
    return false;
  }
  const uint8_t *remoteSeed = resp.data();
  const uint8_t *serverHash = resp.data() + 16;

  uint8_t remote[16];
  memcpy(remote, remoteSeed, 16);
  uint8_t expect[32];
  handshake1Hash(localSeed, remote, authHash_, expect);
  if (memcmp(expect, serverHash, 32) != 0) {
    err_ = "auth failed (check Tapo email/password)";
    return false;
  }

  uint8_t h2[32];
  handshake2Hash(localSeed, remote, authHash_, h2);
  if (!post("/app/handshake2", h2, 32, status, resp, false)) return false;
  if (status != 200) {
    err_ = "handshake2 HTTP " + std::to_string(status);
    return false;
  }

  cipher_.init(localSeed, remote, authHash_);
  uint32_t timeoutS = cookieTimeoutS_ ? cookieTimeoutS_ : 86400;
  timeoutS = timeoutS > kSessionBufferS * 2 ? timeoutS - kSessionBufferS : timeoutS / 2;
  sessionExpiresMs_ = millis() + timeoutS * 1000UL;
  sessionValid_ = true;
  return true;
}

bool KlapClient::sendOnce(const std::string &json, std::string &response, int &status) {
  std::vector<uint8_t> body;
  const int32_t seq = cipher_.encrypt(reinterpret_cast<const uint8_t *>(json.data()), json.size(), body);
  char path[48];
  snprintf(path, sizeof(path), "/app/request?seq=%ld", static_cast<long>(seq));
  std::vector<uint8_t> resp;
  if (!post(path, body.data(), body.size(), status, resp, false)) return false;
  if (status != 200) {
    err_ = "request HTTP " + std::to_string(status);
    return false;
  }
  if (!cipher_.decrypt(resp.data(), resp.size(), response)) {
    err_ = "decrypt failed";
    return false;
  }
  return true;
}

bool KlapClient::call(const char *method, const char *paramsJson, std::string &response) {
  std::string json = "{\"method\":\"";
  json += method;
  json += "\"";
  if (paramsJson && *paramsJson) {
    json += ",\"params\":";
    json += paramsJson;
  }
  json += ",\"request_time_milis\":";
  json += std::to_string(static_cast<unsigned long long>(millis()));
  json += ",\"terminal_uuid\":\"tapo-knob\"}";

  if (sessionValid_ && static_cast<int32_t>(millis() - sessionExpiresMs_) >= 0) sessionValid_ = false;

  for (int attempt = 0; attempt < 2; ++attempt) {
    if (!sessionValid_ && !handshake()) return false;
    int status = 0;
    if (sendOnce(json, response, status)) return true;
    // 403 / decrypt failure => session is stale; retry once with a fresh handshake.
    sessionValid_ = false;
    if (status != 403 && err_ != "decrypt failed") return false;
  }
  return false;
}

}  // namespace tapo
