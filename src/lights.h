// Light service: owns Wi-Fi and all Tapo network I/O in a background FreeRTOS task.
//
// The UI never blocks on the network. It calls the set*() functions, which record the
// desired state (latest wins) and update the cached state optimistically. The task sends
// pending changes at a bounded rate and polls every bulb so changes made from the Tapo app
// show up on the knob.
#pragma once

#include <stdint.h>

namespace lights {

constexpr int kMaxBulbs = 6;
constexpr int kAll = -1;  // target index meaning "every bulb"

enum class NetStatus : uint8_t {
  NotConfigured,  // secrets.h still has placeholders
  Connecting,     // waiting for Wi-Fi
  Online,         // Wi-Fi up
};

struct Bulb {
  bool reachable = false;  // last request succeeded
  bool known = false;      // we have received device info at least once
  bool on = false;
  uint8_t brightness = 0;  // 1-100
  uint16_t hue = 0;        // 0-360
  uint8_t saturation = 0;  // 0-100
  uint16_t colorTemp = 0;  // Kelvin; 0 = colour mode
  uint16_t ctMin = 2500;   // from the device's color_temp_range
  uint16_t ctMax = 6500;
  char name[32] = {};
  char model[16] = {};
  char error[48] = {};
};

// fromSleep: woke from deep sleep; restore cached bulb state, sessions and Wi-Fi AP.
void begin(bool fromSleep);

// True when no changes are waiting to be sent.
bool idle();

// Wi-Fi modem power save (adds latency; used while the screen is off).
void setPowerSave(bool on);

// Stop network activity, persist state to RTC memory and turn Wi-Fi off before deep sleep.
void prepareSleep();

int count();
NetStatus netStatus();

// Incremented whenever any cached state changes, so the UI can cheaply poll for updates.
uint32_t revision();

// Thread-safe copy of a bulb's cached state.
Bulb get(int index);

// target = bulb index or kAll.
void setPower(int target, bool on);
void setBrightness(int target, uint8_t percent);  // also turns the bulb(s) on
void setColorTemp(int target, uint16_t kelvin);   // also turns the bulb(s) on
void setHueSat(int target, uint16_t hue, uint8_t sat);

}  // namespace lights
