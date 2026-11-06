// Put the knob's secondary ESP32 to sleep for good (until power is cycled).
#include "esp_log.h"
#include "esp_sleep.h"

void app_main(void) {
  ESP_LOGI("sleep", "secondary ESP32: entering deep sleep with no wake source");
  // No wake sources are enabled, so only a reset / power cycle brings it back.
  esp_deep_sleep_start();
}
