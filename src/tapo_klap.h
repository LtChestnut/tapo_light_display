// Minimal TP-Link Tapo KLAP v2 client (local HTTP, port 80).
//
// Mirrors python-kasa's KlapTransportV2 + KlapEncryptionSession:
//   auth_hash = SHA256(SHA1(email) || SHA1(password))
//   handshake1: POST /app/handshake1  body=local_seed(16)
//               resp = remote_seed(16) || SHA256(local_seed || remote_seed || auth_hash)
//   handshake2: POST /app/handshake2  body=SHA256(remote_seed || local_seed || auth_hash)
//   x   = local_seed || remote_seed || auth_hash
//   key = SHA256("lsk"||x)[0:16]; ivseq = SHA256("iv"||x); iv = ivseq[0:12],
//   seq = int32_be(ivseq[28:32]); sig = SHA256("ldk"||x)[0:28]
//   request: seq++, AES-128-CBC/PKCS7 with IV = iv || be32(seq)
//            body = SHA256(sig || be32(seq) || ct) || ct  ->  POST /app/request?seq=<seq>
//   response: 32-byte signature || ciphertext (same IV)
//
// Not thread-safe: use one instance per bulb from a single task.
#pragma once

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

namespace tapo {

// auth_hash = SHA256(SHA1(email) || SHA1(password))
void computeAuthHash(const char *email, const char *password, uint8_t out[32]);
// Expected server hash in the handshake1 response: SHA256(local || remote || auth)
void handshake1Hash(const uint8_t local[16], const uint8_t remote[16], const uint8_t auth[32], uint8_t out[32]);
// Body of handshake2: SHA256(remote || local || auth)
void handshake2Hash(const uint8_t local[16], const uint8_t remote[16], const uint8_t auth[32], uint8_t out[32]);

// Plain-old-data snapshot of an established session, so it can live in RTC memory across
// deep sleep (sessions last ~24 h; skipping the handshake saves ~150 ms per bulb on wake).
struct KlapSession {
  bool valid;
  uint8_t key[16];
  uint8_t iv[12];
  uint8_t sig[28];
  int32_t seq;
  char cookie[96];
  int64_t expiresAtS;  // in time(nullptr) seconds, which keep counting through deep sleep
};

// Pure session crypto, split out so it can be unit-tested against python-kasa vectors.
class KlapCipher {
 public:
  void init(const uint8_t localSeed[16], const uint8_t remoteSeed[16], const uint8_t authHash[32]);

  // Increments seq, encrypts, and returns signature||ciphertext in `out`. Returns the seq used.
  int32_t encrypt(const uint8_t *plain, size_t len, std::vector<uint8_t> &out);
  // Decrypts a response (skipping the 32-byte signature) using the IV of the last encrypt().
  bool decrypt(const uint8_t *data, size_t len, std::string &out) const;

  int32_t seq() const { return seq_; }
  const uint8_t *key() const { return key_; }

  void exportTo(KlapSession &s) const;
  void importFrom(const KlapSession &s);

 private:
  uint8_t key_[16] = {};
  uint8_t iv_[12] = {};
  uint8_t sig_[28] = {};
  int32_t seq_ = 0;
};

class KlapClient {
 public:
  void begin(const char *host, const uint8_t authHash[32]);

  // Sends a SMART-protocol request and returns the decrypted JSON response text.
  // Handshakes on demand and re-handshakes once on session errors.
  bool call(const char *method, const char *paramsJson, std::string &response);

  const char *host() const { return host_.c_str(); }
  const char *lastError() const { return err_.c_str(); }
  void invalidate() { sessionValid_ = false; }

  void exportSession(KlapSession &s) const;
  void importSession(const KlapSession &s);

 private:
  bool handshake();
  bool sendOnce(const std::string &json, std::string &response, int &status);
  bool post(const char *path, const uint8_t *body, size_t len, int &status,
            std::vector<uint8_t> &resp, bool captureCookie);

  String host_;
  uint8_t authHash_[32] = {};
  KlapCipher cipher_;
  bool sessionValid_ = false;
  uint32_t sessionExpiresMs_ = 0;
  String cookie_;  // "TP_SESSIONID=..."
  uint32_t cookieTimeoutS_ = 0;
  std::string err_;
};

}  // namespace tapo
