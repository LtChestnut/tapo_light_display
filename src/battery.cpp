#include "battery.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <esp_attr.h>

#include <vector>

#include "config.h"
#include "hal.h"

namespace battery {
namespace {

// USB vs battery, with hysteresis: USB reads ~4.45-4.6 V but sags to ~4.36 V under boot
// load; a full cell rests at <= 4.2 V.
constexpr float kUsbEnterV = 4.30f;
constexpr float kUsbLeaveV = 4.22f;
constexpr uint32_t kSampleMs = 5000;
constexpr uint32_t kLogEveryMs = 30000;
constexpr uint16_t kMinValidEndMv = 3500;    // a run that "ended" above this was a crash, not empty
constexpr uint32_t kMinValidRunS = 30 * 60;  // shorter runs aren't trustworthy
constexpr int kCurvePoints = 11;             // 0, 10, ..., 100 %
const char *const kLogPath = "/cal.bin";

// Generic Li-ion resting curve, used until a calibration has been run.
const uint16_t kDefaultMv[] = {3300, 3500, 3650, 3750, 3850, 3950, 4050, 4150};
const uint8_t kDefaultPct[] = {0, 5, 20, 40, 60, 75, 90, 100};

struct Record {  // one log line in /cal.bin
  uint32_t tS;
  uint16_t mv;
  uint16_t reserved;
};

Preferences s_prefs;  // NVS namespace "battery"
bool s_fsOk = false;

float s_volts = 0;
bool s_onUsb = false;

void updateUsb() {
  if (s_volts >= kUsbEnterV) s_onUsb = true;
  else if (s_volts < kUsbLeaveV) s_onUsb = false;
}
uint32_t s_lastSampleMs = 0;

bool s_hasCurve = false;
uint16_t s_curveMv[kCurvePoints];  // voltage at 0 %, 10 %, ... 100 %

CalState s_cal = CalState::Idle;
uint32_t s_calStartMs = 0;
uint32_t s_lastLogMs = 0;
uint32_t s_samples = 0;
int s_belowUsbCount = 0;
char s_lastResult[96] = "";
String s_cmd;

// Low-battery cutoff. The latch lives in RTC memory, so it survives deep sleep but clears
// on a power cycle or reset.
RTC_DATA_ATTR bool s_lowLatched = false;
bool s_empty = false;
int s_belowCutoffCount = 0;
constexpr int kCutoffSamples = 3;  // 3 x 5 s below the cutoff: ignores Wi-Fi TX dips

bool cutoffApplies(float v) { return LOW_BATT_ON_USB || v < kUsbLeaveV; }

void setEmpty(const char *why) {
  if (s_empty) return;
  s_empty = s_lowLatched = true;
  Serial.printf("[battery] EMPTY (%s, %.3f V): low-battery shutdown\n", why, s_volts);
}

void saveState() { s_prefs.putUChar("calState", static_cast<uint8_t>(s_cal)); }

void loadCurve() {
  s_hasCurve = s_prefs.getBytesLength("curve") == sizeof(s_curveMv) &&
               s_prefs.getBytes("curve", s_curveMv, sizeof(s_curveMv)) == sizeof(s_curveMv);
}

float readVolts() {
  // Average a burst of readings; the divider feeds the ADC directly (no buffer).
  return hal::supplyVolts();
}

int interpolate(float mv, const uint16_t *xs, const uint8_t *ys, int n) {
  if (mv <= xs[0]) return ys[0];
  for (int i = 1; i < n; ++i) {
    if (mv <= xs[i]) return ys[i - 1] + static_cast<int>((mv - xs[i - 1]) / (xs[i] - xs[i - 1]) * (ys[i] - ys[i - 1]));
  }
  return ys[n - 1];
}

// Turn the finished log into the 11-point curve. Assumes a constant load, so charge used is
// proportional to time: SoC(t) = 1 - t / T, with T = time of the last record (death).
bool processLog(char *why, size_t whyLen) {
  File f = LittleFS.exists(kLogPath) ? LittleFS.open(kLogPath, "r") : File();
  if (!f) {
    snprintf(why, whyLen, "no log file");
    return false;
  }
  std::vector<Record> recs(f.size() / sizeof(Record));
  f.read(reinterpret_cast<uint8_t *>(recs.data()), recs.size() * sizeof(Record));
  f.close();
  if (recs.size() < 10) {
    snprintf(why, whyLen, "only %u samples", static_cast<unsigned>(recs.size()));
    return false;
  }
  const uint32_t T = recs.back().tS;
  if (T < kMinValidRunS) {
    snprintf(why, whyLen, "run too short (%lu min)", static_cast<unsigned long>(T / 60));
    return false;
  }
  // Smooth with a centred 5-sample moving average (load spikes from Wi-Fi bursts).
  std::vector<float> mv(recs.size());
  for (size_t i = 0; i < recs.size(); ++i) {
    int lo = i >= 2 ? static_cast<int>(i) - 2 : 0, hi = min<int>(recs.size() - 1, i + 2);
    float sum = 0;
    for (int k = lo; k <= hi; ++k) sum += recs[k].mv;
    mv[i] = sum / (hi - lo + 1);
  }
  if (mv.back() > kMinValidEndMv) {
    snprintf(why, whyLen, "ended at %.2f V, not empty (reset?)", mv.back() / 1000.0f);
    return false;
  }
  uint16_t curve[kCurvePoints];
  for (int p = 0; p < kCurvePoints; ++p) {
    const float socTarget = p * 10.0f;  // %
    // soc decreases along the log; find the segment that brackets socTarget.
    float v = mv.back();
    for (size_t i = 1; i < recs.size(); ++i) {
      const float s0 = 100.0f * (1.0f - static_cast<float>(recs[i - 1].tS) / T);
      const float s1 = 100.0f * (1.0f - static_cast<float>(recs[i].tS) / T);
      if (socTarget <= s0 && socTarget >= s1) {
        const float u = s0 > s1 ? (s0 - socTarget) / (s0 - s1) : 0;
        v = mv[i - 1] + u * (mv[i] - mv[i - 1]);
        break;
      }
      if (i == 1 && socTarget > s0) {
        v = mv[0];
        break;
      }
    }
    curve[p] = static_cast<uint16_t>(v);
  }
  for (int p = 1; p < kCurvePoints; ++p)  // must be monotonic for the lookup
    if (curve[p] < curve[p - 1]) curve[p] = curve[p - 1];

  s_prefs.putBytes("curve", curve, sizeof(curve));
  memcpy(s_curveMv, curve, sizeof(curve));
  s_hasCurve = true;
  snprintf(why, whyLen, "calibrated from a %luh%02lum run (%u samples), %.2f-%.2f V",
           static_cast<unsigned long>(T / 3600), static_cast<unsigned long>((T / 60) % 60),
           static_cast<unsigned>(recs.size()), curve[0] / 1000.0f, curve[10] / 1000.0f);
  return true;
}

void appendRecord() {
  const Record r{(millis() - s_calStartMs) / 1000, static_cast<uint16_t>(s_volts * 1000), 0};
  File f = LittleFS.open(kLogPath, "a");
  if (f) {
    f.write(reinterpret_cast<const uint8_t *>(&r), sizeof(r));
    f.close();  // closed every time so the log survives the battery dying
  }
  ++s_samples;
  Serial.printf("[battery] cal t=%lus %.3f V (%lu samples)\n", static_cast<unsigned long>(r.tS), s_volts,
                static_cast<unsigned long>(s_samples));
}

void printCurve() {
  if (!s_hasCurve) {
    Serial.println("[battery] using the generic Li-ion curve");
    return;
  }
  Serial.print("[battery] calibrated curve (mV at 0,10..100%):");
  for (uint16_t v : s_curveMv) Serial.printf(" %u", v);
  Serial.println();
}

void handleCommand(const String &cmd) {
  if (cmd == "batt low") {
    setEmpty("forced from serial");
    return;
  }
  if (cmd == "cal dump") {
    File f = LittleFS.exists(kLogPath) ? LittleFS.open(kLogPath, "r") : File();
    if (!f) {
      Serial.println("[battery] no log");
      return;
    }
    Serial.println("---- cal.bin begin (t_s,mV) ----");
    Record r;
    while (f.read(reinterpret_cast<uint8_t *>(&r), sizeof(r)) == sizeof(r))
      Serial.printf("%lu,%u\n", static_cast<unsigned long>(r.tS), r.mv);
    Serial.println("---- cal.bin end ----");
    f.close();
  } else if (cmd == "cal status") {
    Serial.printf("[battery] %.3f V, %s, state %u, %lu samples. Last run: %s\n", s_volts,
                  onUsb() ? "USB" : "battery", static_cast<unsigned>(s_cal),
                  static_cast<unsigned long>(s_samples), s_lastResult[0] ? s_lastResult : "-");
    printCurve();
  } else if (cmd == "cal reprocess") {
    char why[96];
    processLog(why, sizeof(why));
    strlcpy(s_lastResult, why, sizeof(s_lastResult));
    s_prefs.putString("lastResult", s_lastResult);
    Serial.printf("[battery] %s\n", why);
    printCurve();
#ifdef CAL_SELFTEST
  } else if (cmd == "cal synth") {  // test only: overwrite /cal.bin with a linear 4 h discharge
    File f = LittleFS.open(kLogPath, "w");
    for (uint32_t t = 0; t <= 4 * 3600; t += 30) {
      const Record r{t, static_cast<uint16_t>(4180 - 880.0f * t / (4 * 3600)), 0};
      f.write(reinterpret_cast<const uint8_t *>(&r), sizeof(r));
    }
    f.close();
    Serial.println("[battery] wrote synthetic log");
#endif
  } else if (cmd == "cal reset") {
    s_prefs.remove("curve");
    s_prefs.remove("lastResult");
    LittleFS.remove(kLogPath);
    s_hasCurve = false;
    s_lastResult[0] = '\0';
    Serial.println("[battery] curve, log and last result cleared; back to the generic Li-ion curve");
  } else if (cmd.length()) {
    Serial.println("[battery] commands: batt low (simulate the low-battery cutoff)");
    Serial.println("[battery] commands: cal status | cal dump | cal reprocess | cal reset (clears curve + log)");
  }
}

}  // namespace

void begin() {
  s_prefs.begin("battery", false);
  s_fsOk = LittleFS.begin(true /*format if needed*/, "/littlefs", 5, "spiffs");
  if (!s_fsOk) Serial.println("[battery] LittleFS mount failed; calibration unavailable");
  loadCurve();
  strlcpy(s_lastResult, s_prefs.getString("lastResult", "").c_str(), sizeof(s_lastResult));
  s_volts = readVolts();
  s_onUsb = s_volts >= (kUsbEnterV + kUsbLeaveV) / 2;
  s_lastSampleMs = millis();

  // A run that was still going when we last stopped means the battery died (or we crashed):
  // turn the log into a curve now.
  const auto prev = static_cast<CalState>(s_prefs.getUChar("calState", 0));
  if (prev == CalState::Running && s_fsOk) {
    char why[96];
    const bool ok = processLog(why, sizeof(why));
    Serial.printf("[battery] calibration run finished: %s\n", why);
    snprintf(s_lastResult, sizeof(s_lastResult), "%s%s", ok ? "" : "rejected: ", why);
    s_prefs.putString("lastResult", s_lastResult);
  }
  s_cal = CalState::Idle;
  saveState();
  printCurve();
}

void service() {
  // Serial commands (line based).
  while (Serial.available()) {
    const char c = static_cast<char>(Serial.read());
    if (c == '\n' || c == '\r') {
      s_cmd.trim();
      handleCommand(s_cmd);
      s_cmd = "";
    } else if (s_cmd.length() < 40) {
      s_cmd += c;
    }
  }

  const uint32_t now = millis();
  if (now - s_lastSampleMs >= kSampleMs) {
    s_lastSampleMs = now;
    s_volts = s_volts * 0.7f + readVolts() * 0.3f;  // light smoothing
    updateUsb();
    if (cutoffApplies(s_volts) && s_volts * 1000.0f < LOW_BATT_CUTOFF_MV) {
      if (++s_belowCutoffCount >= kCutoffSamples) setEmpty("below cutoff");
    } else {
      s_belowCutoffCount = 0;
    }
  }

  switch (s_cal) {
    case CalState::Armed:
      // Start the clock once we're genuinely on battery (two readings in a row).
      if (now == s_lastSampleMs) {
        s_belowUsbCount = !s_onUsb ? s_belowUsbCount + 1 : 0;
        if (s_belowUsbCount >= 2) {
          LittleFS.remove(kLogPath);
          s_calStartMs = s_lastLogMs = now;
          s_samples = 0;
          s_cal = CalState::Running;
          saveState();
          appendRecord();
          Serial.println("[battery] calibration started (on battery)");
        }
      }
      break;
    case CalState::Running:
      if (now - s_lastLogMs >= kLogEveryMs) {
        s_lastLogMs = now;
        appendRecord();
      }
      break;
    default:
      break;
  }
}

float volts() { return s_volts; }
bool onUsb() { return s_onUsb; }
bool calibrated() { return s_hasCurve; }

bool bootIsEmpty() {
  const float v = hal::supplyVolts();  // plain ADC read; nothing else is running yet
  if (!cutoffApplies(v) || v * 1000.0f >= LOW_BATT_RECOVER_MV) {
    s_lowLatched = false;  // charged (or on USB): back to normal
    return false;
  }
  // Latched: stay off until it has recovered. Not latched: only a cell already below the
  // cutoff with no load on it counts; a marginal one gets caught by the loaded check.
  if (s_lowLatched || v * 1000.0f < LOW_BATT_CUTOFF_MV) {
    s_lowLatched = true;
    Serial.printf("[battery] %.3f V at boot: still empty\n", v);
    return true;
  }
  return false;
}

bool empty() { return s_empty; }

void clearEmpty() {
  s_empty = s_lowLatched = false;
  s_belowCutoffCount = 0;
}

namespace {
int rawPercent(float mv) {
  if (s_hasCurve) {
    static const uint8_t kPct[kCurvePoints] = {0, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100};
    return interpolate(mv, s_curveMv, kPct, kCurvePoints);
  }
  return interpolate(mv, kDefaultMv, kDefaultPct, sizeof(kDefaultPct));
}
}  // namespace

int percent() {
  // Rescale so the cutoff is 0 %: the curve's own 0 % is where the cell actually died.
  const int cut = rawPercent(LOW_BATT_CUTOFF_MV);
  const int raw = rawPercent(s_volts * 1000.0f);
  if (raw <= cut) return 0;
  return cut >= 100 ? 100 : (raw - cut) * 100 / (100 - cut);
}

CalState calState() { return s_cal; }

void calStart() {
  if (!s_fsOk) return;
  s_cal = CalState::Armed;
  s_belowUsbCount = 0;
  saveState();
  Serial.println("[battery] calibration armed: unplug USB to start");
}

void calStop() {
  if (s_cal == CalState::Running) {
    snprintf(s_lastResult, sizeof(s_lastResult), "stopped by user after %lu min",
             static_cast<unsigned long>(calElapsedS() / 60));
    s_prefs.putString("lastResult", s_lastResult);
  }
  s_cal = CalState::Idle;
  saveState();
  Serial.println("[battery] calibration stopped");
}

uint32_t calElapsedS() { return s_cal == CalState::Running ? (millis() - s_calStartMs) / 1000 : 0; }
uint32_t calSamples() { return s_samples; }
const char *calLastResult() { return s_lastResult; }

}  // namespace battery
