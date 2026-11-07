#include <Arduino.h>
#include <esp_sleep.h>
#include <lvgl.h>

#include "battery.h"
#include "config.h"
#include "hal.h"
#include "lights.h"
#include "ui.h"

void setup() {
  const esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  const bool fromSleep = cause == ESP_SLEEP_WAKEUP_EXT1;

  Serial.begin(115200);
  if (!fromSleep) delay(200);  // let USB CDC enumerate for the boot log; skip on wake
  Serial.printf("[app] Tapo light knob starting (%s)\n", fromSleep ? "woke from deep sleep" : "cold boot");
  if (fromSleep) {
    const uint64_t pins = esp_sleep_get_ext1_wakeup_status();
    Serial.printf("[app] wake pin mask 0x%llx\n", pins);
  }

  // Start Wi-Fi first so it joins while the display initialises (~1 s) on the other core.
  lights::begin(fromSleep);  // Wi-Fi + Tapo I/O run on core 0; LVGL stays on this loop (core 1)
  hal::begin();
  battery::begin();  // after hal (ADC); finishes a calibration run the battery ended
  Serial.printf("[app] v%s #%s, supply %.2f V, haptics %s\n", FW_VERSION, GIT_HASH, battery::volts(),
                hal::hasHaptics() ? "yes" : "no");
  ui::begin();
  Serial.printf("[app] UI up at %lu ms after boot\n", millis());
}

void loop() {
  static uint32_t lastSupplyLog = 0;
  if (millis() - lastSupplyLog > 60000) {
    lastSupplyLog = millis();
    Serial.printf("[app] supply %.2f V\n", hal::supplyVolts());
  }
  hal::service();
  battery::service();
  ui::tick(hal::takeKnobDelta());
  lv_timer_handler();

  if (ui::wantsDeepSleep()) {
    lights::prepareSleep();     // flushes nothing new; persists state and turns Wi-Fi off
    hal::deepSleep(TOUCH_WAKE);  // does not return; wake = reboot
  }
  delay(5);
}
