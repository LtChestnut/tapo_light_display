// User-tunable behaviour. Build-time overrides: -D<NAME>=<value> in platformio.ini or
// PLATFORMIO_BUILD_FLAGS.
#pragma once

// Backlight off after this long without a touch or knob detent.
#ifndef SCREEN_TIMEOUT_MS
#define SCREEN_TIMEOUT_MS (20 * 1000)
#endif

// Deep sleep after this long without input (counted from the last input, so the screen has
// been off for DEEP_SLEEP_AFTER_MS - SCREEN_TIMEOUT_MS). Until then Wi-Fi stays connected
// in power-save mode, so a quick follow-up responds instantly.
#ifndef DEEP_SLEEP_AFTER_MS
#define DEEP_SLEEP_AFTER_MS (45 * 1000)
#endif

// Deep sleep only on battery by default. On USB it stays awake (screen off), which keeps
// the USB serial port alive for flashing and logs. Set to 1 to test sleep on the bench.
#ifndef DEEP_SLEEP_ON_USB
#define DEEP_SLEEP_ON_USB 0
#endif

// Wake from deep sleep by touching the screen (as well as twisting the knob). Keeps the
// touch controller powered in its low-power detect mode, so the sleep current is higher.
// Default off until the battery drain has been measured both ways.
#ifndef TOUCH_WAKE
#define TOUCH_WAKE 0
#endif

// Low-battery cutoff, on battery only. Below LOW_BATT_CUTOFF_MV (under load, 3 readings
// 5 s apart) the knob shows "Battery low" and deep sleeps. The calibration run (2026-11-07)
// reached 3.3 V at 93 % of its runtime and fell off a cliff after it. Twisting the knob
// still wakes it, but until the cell rests above LOW_BATT_RECOVER_MV (or USB is plugged
// in) it only shows the message and goes back to sleep, without starting Wi-Fi. The battery %
// is scaled so that 0 % = the cutoff.
#ifndef LOW_BATT_CUTOFF_MV
#define LOW_BATT_CUTOFF_MV 3300
#endif
#ifndef LOW_BATT_RECOVER_MV
#define LOW_BATT_RECOVER_MV 3450  // an empty cell rebounds ~0.1-0.2 V once the load is off
#endif
#ifndef LOW_BATT_ON_USB  // 1 = apply the cutoff on USB too (bench testing with a high cutoff)
#define LOW_BATT_ON_USB 0
#endif
#ifndef LOW_BATT_MESSAGE_MS
#define LOW_BATT_MESSAGE_MS 3000
#endif

// Firmware version shown in the secret menu (the git hash is added at build time).
#define FW_VERSION "1.1"
