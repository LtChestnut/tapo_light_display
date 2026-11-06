#include "lights.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <esp_attr.h>
#include <mbedtls/base64.h>

#include <atomic>

#include "secrets.h"
#include "tapo_klap.h"

namespace lights {
namespace {

constexpr uint32_t kMinSendIntervalMs = 150;  // per bulb, while the knob is turning
constexpr uint32_t kPollIntervalMs = 5000;    // pick up changes made from the Tapo app
constexpr uint32_t kRetryBackoffMs = 5000;    // after a failed request
constexpr uint8_t kMaxSendFailures = 2;       // then drop the pending change

enum : uint8_t { kPendOn = 1, kPendBri = 2, kPendCt = 4, kPendHs = 8 };

struct Pending {
  uint8_t mask = 0;
  bool on = false;
  uint8_t bri = 0;
  uint16_t ct = 0;
  uint16_t hue = 0;
  uint8_t sat = 0;
  uint8_t failures = 0;
};

struct Slot {
  Bulb bulb;
  Pending pend;
  uint32_t gen = 0;  // bumped on every local change
  uint32_t lastSendMs = 0;
  uint32_t nextPollMs = 0;
  uint32_t retryAtMs = 0;
  tapo::KlapClient client;
};

const char *const kIps[] = TAPO_BULB_IPS;
constexpr int kCount = static_cast<int>(sizeof(kIps) / sizeof(kIps[0])) < kMaxBulbs
                           ? static_cast<int>(sizeof(kIps) / sizeof(kIps[0]))
                           : kMaxBulbs;

Slot s_slots[kCount];
SemaphoreHandle_t s_mu = nullptr;
std::atomic<uint32_t> s_rev{1};
std::atomic<NetStatus> s_net{NetStatus::Connecting};

// ---- Deep-sleep persistence --------------------------------------------------
// RTC slow memory survives deep sleep (not power loss). RTC_NOINIT so nothing re-initialises
// it on boot; validity is checked with a magic + layout size.
struct RtcState {
  uint32_t magic;
  uint32_t size;
  uint8_t count;
  uint8_t bssid[6];
  int32_t channel;
  Bulb bulbs[kMaxBulbs];
  tapo::KlapSession sessions[kMaxBulbs];
};
constexpr uint32_t kRtcMagic = 0x7A90C0DE;
// Raw bytes, so no C++ constructor can ever run over it at boot.
RTC_NOINIT_ATTR alignas(8) uint8_t s_rtcRaw[sizeof(RtcState)];
RtcState &s_rtc = *reinterpret_cast<RtcState *>(s_rtcRaw);

uint8_t s_apBssid[6] = {};
int32_t s_apChannel = 0;
bool s_fromSleep = false;
uint32_t s_firstSetLoggedMs = 0;

std::atomic<bool> s_parkReq{false};
std::atomic<bool> s_parked{false};

struct Lock {
  Lock() { xSemaphoreTake(s_mu, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(s_mu); }
};

void bump() { s_rev.fetch_add(1); }

bool configured() {
  return strcmp(WIFI_SSID, "your-ssid") != 0 && strcmp(TAPO_EMAIL, "you@example.com") != 0;
}

bool timeReached(uint32_t now, uint32_t t) { return static_cast<int32_t>(now - t) >= 0; }

void decodeNickname(const char *b64, char *out, size_t outLen) {
  size_t n = 0;
  if (!b64 || mbedtls_base64_decode(reinterpret_cast<uint8_t *>(out), outLen - 1, &n,
                                    reinterpret_cast<const uint8_t *>(b64), strlen(b64)) != 0) {
    n = 0;
  }
  out[n] = '\0';
}

// Returns the device error_code (0 = ok) or INT_MIN on transport/parse failure.
int callDevice(Slot &s, const char *method, const char *params, JsonDocument *docOut) {
  std::string resp;
  if (!s.client.call(method, params, resp)) return INT32_MIN;
  JsonDocument local;
  JsonDocument &doc = docOut ? *docOut : local;
  if (deserializeJson(doc, resp)) return INT32_MIN;
  return doc["error_code"] | INT32_MIN;
}

void markFailure(int i, const char *what) {
  Slot &s = s_slots[i];
  Lock l;
  if (s.bulb.reachable || strcmp(s.bulb.error, what) != 0) {
    Serial.printf("[lights] %s: %s\n", kIps[i], what);
  }
  s.bulb.reachable = false;
  strlcpy(s.bulb.error, what, sizeof(s.bulb.error));
  s.retryAtMs = millis() + kRetryBackoffMs;
  bump();
}

void sendPending(int i, const Pending &p, uint32_t gen) {
  Slot &s = s_slots[i];
  char params[128];
  int n = snprintf(params, sizeof(params), "{");
  auto add = [&](const char *fmt, auto... v) {
    n += snprintf(params + n, sizeof(params) - n, "%s", n > 1 ? "," : "");
    n += snprintf(params + n, sizeof(params) - n, fmt, v...);
  };
  if (p.mask & kPendOn) add("\"device_on\":%s", p.on ? "true" : "false");
  if (p.mask & kPendBri) add("\"brightness\":%u", p.bri);
  if (p.mask & kPendCt) add("\"color_temp\":%u", p.ct);
  if (p.mask & kPendHs) add("\"hue\":%u,\"saturation\":%u,\"color_temp\":0", p.hue, p.sat);
  snprintf(params + n, sizeof(params) - n, "}");

  const int ec = callDevice(s, "set_device_info", params, nullptr);
  s.lastSendMs = millis();
  if (ec == 0) {
    if (!s_firstSetLoggedMs) {
      s_firstSetLoggedMs = millis();
      Serial.printf("[lights] first command delivered at %lu ms after boot\n", s_firstSetLoggedMs);
    }
    Lock l;
    if (s.gen == gen) s.pend.mask = 0;  // else newer changes arrived; keep them queued
    s.pend.failures = 0;
    s.bulb.reachable = true;
    s.bulb.error[0] = '\0';
    bump();
    return;
  }

  char what[48];
  if (ec == INT32_MIN) snprintf(what, sizeof(what), "%s", s.client.lastError());
  else snprintf(what, sizeof(what), "device error %d", ec);
  {
    Lock l;
    if (++s.pend.failures >= kMaxSendFailures) {
      s.pend = Pending{};
      s.nextPollMs = 0;  // resync from the device when it comes back
    }
  }
  markFailure(i, what);
}

void poll(int i) {
  Slot &s = s_slots[i];
  uint32_t genAtStart;
  {
    Lock l;
    genAtStart = s.gen;
  }
  JsonDocument doc;
  const int ec = callDevice(s, "get_device_info", nullptr, &doc);
  s.nextPollMs = millis() + kPollIntervalMs;
  if (ec != 0) {
    char what[48];
    if (ec == INT32_MIN) snprintf(what, sizeof(what), "%s", s.client.lastError());
    else snprintf(what, sizeof(what), "device error %d", ec);
    markFailure(i, what);
    return;
  }

  JsonObject r = doc["result"];
  Lock l;
  Bulb &b = s.bulb;
  if (!b.known) {
    char nick[sizeof(b.name)];
    decodeNickname(r["nickname"] | "", nick, sizeof(nick));
    if (nick[0]) strlcpy(b.name, nick, sizeof(b.name));
    strlcpy(b.model, r["model"] | "", sizeof(b.model));
    JsonArrayConst range = r["color_temp_range"];
    if (range.size() == 2 && range[0].as<int>() > 0 && range[1].as<int>() > range[0].as<int>()) {
      b.ctMin = range[0].as<uint16_t>();
      b.ctMax = range[1].as<uint16_t>();
    }
    Serial.printf("[lights] %s: \"%s\" %s online\n", kIps[i], b.name, b.model);
  }
  b.known = true;
  b.reachable = true;
  b.error[0] = '\0';
  // Don't clobber optimistic UI state with a reading taken before the user's latest change.
  if (s.gen == genAtStart && s.pend.mask == 0) {
    b.on = r["device_on"] | false;
    b.brightness = r["brightness"] | 0;
    b.hue = r["hue"] | 0;
    b.saturation = r["saturation"] | 0;
    b.colorTemp = r["color_temp"] | 0;
  }
  bump();
}

void serviceTask(void *) {
  if (!s_fromSleep) {  // after deep sleep, begin() already set the clients up
    uint8_t authHash[32];
    tapo::computeAuthHash(TAPO_EMAIL, TAPO_PASSWORD, authHash);
    for (int i = 0; i < kCount; ++i) s_slots[i].client.begin(kIps[i], authHash);
    memset(authHash, 0, sizeof(authHash));
  }

  WiFi.mode(WIFI_STA);
  WiFi.setHostname("tapo-knob");
  WiFi.setSleep(false);  // power save adds 100+ ms latency to every request
  WiFi.setAutoReconnect(true);
  // After deep sleep, go straight to the known AP/channel instead of scanning (~1-2 s saved).
  const bool fast = s_apChannel > 0;
  if (fast) WiFi.begin(WIFI_SSID, WIFI_PASSWORD, s_apChannel, s_apBssid, true);
  else WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.printf("[lights] connecting to Wi-Fi \"%s\"%s\n", WIFI_SSID, fast ? " (cached AP)" : "");
  const uint32_t wifiStartMs = millis();
  bool fellBack = !fast;

  for (;;) {
    if (s_parkReq.load()) {  // deep sleep requested: stop touching the clients
      s_parked = true;
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }
    if (WiFi.status() != WL_CONNECTED) {
      if (!fellBack && millis() - wifiStartMs > 4000) {  // AP moved / changed channel
        fellBack = true;
        Serial.println("[lights] cached AP failed; scanning");
        WiFi.disconnect();
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
      }
      if (s_net.exchange(NetStatus::Connecting) != NetStatus::Connecting) {
        Serial.println("[lights] Wi-Fi lost");
        for (auto &s : s_slots) s.client.invalidate();
        bump();
      }
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }
    if (s_net.exchange(NetStatus::Online) != NetStatus::Online) {
      Serial.printf("[lights] Wi-Fi up at %lu ms after boot, IP %s\n", millis(),
                    WiFi.localIP().toString().c_str());
      memcpy(s_apBssid, WiFi.BSSID(), 6);
      s_apChannel = WiFi.channel();
      bump();
    }

    bool worked = false;
    for (int i = 0; i < kCount; ++i) {
      Slot &s = s_slots[i];
      const uint32_t now = millis();
      if (!timeReached(now, s.retryAtMs)) continue;

      Pending p;
      uint32_t gen;
      {
        Lock l;
        p = s.pend;
        gen = s.gen;
      }
      if (p.mask) {
        if (timeReached(now, s.lastSendMs + kMinSendIntervalMs)) {
          sendPending(i, p, gen);
          worked = true;
        }
      } else if (timeReached(now, s.nextPollMs)) {
        poll(i);
        worked = true;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(worked ? 1 : 20));
  }
}

template <typename F>
void forTargets(int target, F &&fn) {
  Lock l;
  for (int i = 0; i < kCount; ++i) {
    if (target != kAll && target != i) continue;
    fn(s_slots[i]);
    s_slots[i].gen++;
  }
  bump();
}

}  // namespace

void begin(bool fromSleep) {
  s_mu = xSemaphoreCreateMutex();
  for (int i = 0; i < kCount; ++i) {
    snprintf(s_slots[i].bulb.name, sizeof(s_slots[i].bulb.name), "Light %d", i + 1);
  }
  s_fromSleep = fromSleep && s_rtc.magic == kRtcMagic && s_rtc.size == sizeof(RtcState) &&
                s_rtc.count == kCount;
  if (s_fromSleep) {
    // Show the last known state immediately and reuse the bulb sessions (no handshakes).
    uint8_t authHash[32];
    tapo::computeAuthHash(TAPO_EMAIL, TAPO_PASSWORD, authHash);
    for (int i = 0; i < kCount; ++i) {
      s_slots[i].bulb = s_rtc.bulbs[i];
      s_slots[i].client.begin(kIps[i], authHash);
      s_slots[i].client.importSession(s_rtc.sessions[i]);
    }
    memset(authHash, 0, sizeof(authHash));
    memcpy(s_apBssid, s_rtc.bssid, 6);
    s_apChannel = s_rtc.channel;
  }
  s_rtc.magic = 0;  // consumed; prepareSleep() writes it again
  if (!configured()) {
    s_net = NetStatus::NotConfigured;
    Serial.println("[lights] src/secrets.h still has placeholder values; network disabled");
    return;
  }
  xTaskCreatePinnedToCore(serviceTask, "lights", 12 * 1024, nullptr, 3, nullptr, 0);
}

int count() { return kCount; }

bool idle() {
  if (s_net.load() != NetStatus::Online) return true;  // nothing can be sent anyway
  Lock l;
  for (const Slot &s : s_slots)
    if (s.pend.mask) return false;
  return true;
}

void setPowerSave(bool on) {
  if (s_net.load() != NetStatus::NotConfigured) WiFi.setSleep(on);
}

void prepareSleep() {
  const bool net = s_net.load() != NetStatus::NotConfigured;
  if (net) {
    s_parkReq = true;
    const uint32_t t0 = millis();
    while (!s_parked.load() && millis() - t0 < 5000) delay(10);
  }
  s_rtc.size = sizeof(RtcState);
  s_rtc.count = kCount;
  memcpy(s_rtc.bssid, s_apBssid, 6);
  s_rtc.channel = s_apChannel;
  {
    Lock l;
    for (int i = 0; i < kCount; ++i) {
      s_rtc.bulbs[i] = s_slots[i].bulb;
      s_slots[i].client.exportSession(s_rtc.sessions[i]);
    }
  }
  s_rtc.magic = kRtcMagic;
  if (net) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }
}
NetStatus netStatus() { return s_net.load(); }
uint32_t revision() { return s_rev.load(); }

Bulb get(int index) {
  if (index < 0 || index >= kCount) return Bulb{};
  Lock l;
  return s_slots[index].bulb;
}

void setPower(int target, bool on) {
  forTargets(target, [&](Slot &s) {
    s.pend.mask |= kPendOn;
    s.pend.on = on;
    s.bulb.on = on;
  });
}

void setBrightness(int target, uint8_t percent) {
  percent = percent < 1 ? 1 : (percent > 100 ? 100 : percent);
  forTargets(target, [&](Slot &s) {
    s.pend.mask |= kPendBri | kPendOn;
    s.pend.bri = percent;
    s.pend.on = true;
    s.bulb.brightness = percent;
    s.bulb.on = true;
  });
}

void setColorTemp(int target, uint16_t kelvin) {
  forTargets(target, [&](Slot &s) {
    s.pend.mask = (s.pend.mask & ~kPendHs) | kPendCt | kPendOn;
    s.pend.ct = kelvin;
    s.pend.on = true;
    s.bulb.colorTemp = kelvin;
    s.bulb.on = true;
  });
}

void setHueSat(int target, uint16_t hue, uint8_t sat) {
  forTargets(target, [&](Slot &s) {
    s.pend.mask = (s.pend.mask & ~kPendCt) | kPendHs | kPendOn;
    s.pend.hue = hue;
    s.pend.sat = sat;
    s.pend.on = true;
    s.bulb.hue = hue;
    s.bulb.saturation = sat;
    s.bulb.colorTemp = 0;
    s.bulb.on = true;
  });
}

}  // namespace lights
