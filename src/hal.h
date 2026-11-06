// Hardware layer: display + touch + backlight + knob + haptics + battery, wired into LVGL.
#pragma once
#include <stdint.h>

namespace hal {

// Brings up backlight, touch, LCD, LVGL display/input drivers, the knob and haptics.
void begin();

// 0-100
void setBacklight(uint8_t percent);

// Power down display/touch/haptics, arm knob (and optionally touch) wake, enter deep sleep.
// Does not return; the chip reboots on wake.
[[noreturn]] void deepSleep(bool touchWake);

// Knob detents since the last call (+ = clockwise).
int32_t takeKnobDelta();

// Housekeeping hook called from loop() (currently nothing).
void service();

// Haptic feedback (no-op if the DRV2605 didn't answer on the I2C bus).
enum class Haptic : uint8_t { Tick, Click, Bump };
void haptic(Haptic h);
bool hasHaptics();

// Supply voltage on the "5V" rail, which is the battery when USB is unplugged.
// Reads ~4.6-5 V while on USB power.
float supplyVolts();

}  // namespace hal
