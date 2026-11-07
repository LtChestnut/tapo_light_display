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

// Firmware version shown in the secret menu (the git hash is added at build time).
#define FW_VERSION "1.0"
