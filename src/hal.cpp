#include "hal.h"

#include <Arduino.h>
#include <ESP_Panel_Library.h>
#include <lvgl.h>
#include <atomic>

#include "board_pins.h"
#include "st77916_init.h"
#include <driver/i2c.h>
#include <driver/rtc_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_sleep.h>
#include <esp_timer.h>

namespace hal {
namespace {

constexpr uint32_t kQspiFreqHz = 50 * 1000 * 1000;
constexpr size_t kDrawBufRows = 72;

ESP_PanelBacklightPWM_LEDC *s_backlight = nullptr;
ESP_PanelLcd *s_lcd = nullptr;
ESP_PanelBusQSPI *s_lcdBus = nullptr;
ESP_PanelTouch *s_touch = nullptr;

lv_disp_draw_buf_t s_drawBuf;
lv_disp_drv_t s_dispDrv;
lv_indev_drv_t s_touchDrv;

std::atomic<int32_t> s_knobDelta{0};

// ---- LVGL glue ------------------------------------------------------------

void flushCb(lv_disp_drv_t *drv, const lv_area_t *a, lv_color_t *px) {
  auto *lcd = static_cast<ESP_PanelLcd *>(drv->user_data);
  lcd->drawBitmap(a->x1, a->y1, a->x2 - a->x1 + 1, a->y2 - a->y1 + 1,
                  reinterpret_cast<const uint8_t *>(px));
  // lv_disp_flush_ready() is called from the DMA-done callback below.
}

IRAM_ATTR bool onDrawDone(void *user) {
  lv_disp_flush_ready(static_cast<lv_disp_drv_t *>(user));
  return false;
}

// Press-and-hold tracking for the secret menu (independent of which LVGL object is hit).
bool s_touchDown = false;
bool s_touchMoved = false;
uint32_t s_touchStartMs = 0;
int s_touchX0 = 0, s_touchY0 = 0;
constexpr int kHoldMaxMovePx = 25;

void touchReadCb(lv_indev_drv_t *drv, lv_indev_data_t *data) {
  auto *tp = static_cast<ESP_PanelTouch *>(drv->user_data);
  ESP_PanelTouchPoint pt;
  if (tp->readPoints(&pt, 1) > 0) {
    data->point.x = pt.x;
    data->point.y = pt.y;
    data->state = LV_INDEV_STATE_PRESSED;
    if (!s_touchDown) {
      s_touchDown = true;
      s_touchMoved = false;
      s_touchStartMs = millis();
      s_touchX0 = pt.x;
      s_touchY0 = pt.y;
    } else {
      const int dx = pt.x - s_touchX0, dy = pt.y - s_touchY0;
      if (dx * dx + dy * dy > kHoldMaxMovePx * kHoldMaxMovePx) s_touchMoved = true;
    }
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
    s_touchDown = false;
  }
}

// ---- Knob ---------------------------------------------------------------
//
// Measured with an edge trace (see CLAUDE.md): each detent is ONE long low pulse on one pin
// (A = GPIO8 clockwise, B = GPIO7 anticlockwise; 24-300 ms depending on speed). When that
// pin releases, the OTHER pin shows a ~0.1 ms burst of crosstalk spikes ~0.2 ms later.
// Real pulses are never shorter than ~20 ms and the spikes never longer than 1 ms.
//
// So: sample both pins every 1 ms. A pin that reads low for kLowSamples in a row counts
// one detent in its direction and is then disarmed until it reads high for kHighSamples in
// a row. Spikes can't satisfy that; real detents register ~3 ms after they start.
// (Edge-interrupt decoders kept getting fooled by the spikes.)
constexpr uint8_t kLowSamples = 3;
constexpr uint8_t kHighSamples = 3;

struct KnobChan {
  gpio_num_t pin;
  int8_t dir;
  uint8_t lowRun;
  uint8_t highRun;
  bool armed;
};
KnobChan s_chans[2] = {{static_cast<gpio_num_t>(PIN_ENC_A), +1, 0, kHighSamples, true},
                       {static_cast<gpio_num_t>(PIN_ENC_B), -1, 0, kHighSamples, true}};
esp_timer_handle_t s_knobTimer = nullptr;

void knobSample(void *) {
  for (KnobChan &c : s_chans) {
    if (gpio_get_level(c.pin) == 0) {
      c.highRun = 0;
      if (c.lowRun < 255) ++c.lowRun;
      if (c.armed && c.lowRun >= kLowSamples) {
        c.armed = false;
        s_knobDelta.fetch_add(c.dir);
      }
    } else {
      c.lowRun = 0;
      if (c.highRun < 255) ++c.highRun;
      if (c.highRun >= kHighSamples) c.armed = true;
    }
  }
}

// ---- Haptics (DRV2605/DRV2605L on the touch I2C bus) ----------------------

constexpr uint8_t kDrvAddr = 0x5A;
constexpr i2c_port_t kI2cPort = I2C_NUM_0;  // installed by ESP32_Display_Panel (legacy driver)
constexpr int kHapticEnPinV1 = 38;          // DRV2605 EN on V1.0 boards (tied high on V2.0)
bool s_haptics = false;
esp_timer_handle_t s_hapticOff = nullptr;

bool drvWrite(uint8_t reg, uint8_t val) {
  const uint8_t buf[2] = {reg, val};
  return i2c_master_write_to_device(kI2cPort, kDrvAddr, buf, 2, pdMS_TO_TICKS(20)) == ESP_OK;
}

bool drvRead(uint8_t reg, uint8_t &val) {
  return i2c_master_write_read_device(kI2cPort, kDrvAddr, &reg, 1, &val, 1, pdMS_TO_TICKS(20)) == ESP_OK;
}

void initHaptics() {
  uint8_t status = 0;
  if (!drvRead(0x00, status)) {
    // V1.0 boards gate the driver with GPIO38.
    pinMode(kHapticEnPinV1, OUTPUT);
    digitalWrite(kHapticEnPinV1, HIGH);
    delay(5);
    if (!drvRead(0x00, status)) {
      Serial.println("[hal] DRV2605 not found at 0x5A; haptics disabled");
      return;
    }
  }
  uint8_t fb = 0;
  s_haptics = drvWrite(0x01, 0x00)                 // MODE: out of standby
              && drvRead(0x1A, fb)                 // FEEDBACK
              && drvWrite(0x1A, fb | 0x80)         //   N_ERM_LRA = 1 (LRA actuator)
              && drvWrite(0x1D, 0x01)              // CONTROL3: LRA open loop, signed RTP data
              && drvWrite(0x02, 0x00)              // RTP amplitude 0
              && drvWrite(0x01, 0x05);             // MODE: real-time playback
  // Library effects ring for 30-60 ms on this LRA; RTP lets us cut the drive after a few ms.
  const esp_timer_create_args_t args = {.callback = [](void *) { drvWrite(0x02, 0x00); },
                                        .arg = nullptr,
                                        .dispatch_method = ESP_TIMER_TASK,
                                        .name = "haptic_off",
                                        .skip_unhandled_events = true};
  if (s_haptics) s_haptics = esp_timer_create(&args, &s_hapticOff) == ESP_OK;
  Serial.printf("[hal] DRV2605 id %u %s\n", status >> 5, s_haptics ? "ready" : "init failed");
}

// ---- Bring-up -------------------------------------------------------------

void initBacklight() {
  gpio_hold_dis(static_cast<gpio_num_t>(PIN_LCD_BL));  // held low through deep sleep
  s_backlight = new ESP_PanelBacklightPWM_LEDC(PIN_LCD_BL, true);
  s_backlight->begin();
  s_backlight->off();
}

void initTouch() {
  auto *bus = new ESP_PanelBusI2C(PIN_TP_SCL, PIN_TP_SDA, ESP_LCD_TOUCH_IO_I2C_CST816S_CONFIG());
  bus->configI2cFreqHz(400000);
  if (!bus->begin()) Serial.println("[hal] touch I2C bus begin failed");

  s_touch = new ESP_PanelTouch_CST816S(bus, LCD_WIDTH, LCD_HEIGHT, PIN_TP_RST, PIN_TP_INT);
  s_touch->init();
  s_touch->begin();
}

void initLcd() {
  auto *bus = new ESP_PanelBusQSPI(PIN_LCD_CS, PIN_LCD_SCK, PIN_LCD_D0, PIN_LCD_D1, PIN_LCD_D2, PIN_LCD_D3);
  bus->configQspiFreqHz(kQspiFreqHz);
  bus->begin();
  s_lcdBus = bus;

  s_lcd = new ESP_PanelLcd_ST77916(bus, 16, PIN_LCD_RST);
  s_lcd->configVendorCommands(kSt77916InitCmds, sizeof(kSt77916InitCmds) / sizeof(kSt77916InitCmds[0]));
  s_lcd->init();
  s_lcd->reset();
  s_lcd->begin();
  s_lcd->invertColor(true);
  s_lcd->displayOn();
}

void initLvgl() {
  lv_init();

  const size_t px = LCD_WIDTH * kDrawBufRows;
  auto *buf = static_cast<lv_color_t *>(
      heap_caps_malloc(px * sizeof(lv_color_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
  assert(buf);
  lv_disp_draw_buf_init(&s_drawBuf, buf, nullptr, px);

  lv_disp_drv_init(&s_dispDrv);
  s_dispDrv.hor_res = LCD_WIDTH;
  s_dispDrv.ver_res = LCD_HEIGHT;
  s_dispDrv.flush_cb = flushCb;
  s_dispDrv.draw_buf = &s_drawBuf;
  s_dispDrv.user_data = s_lcd;
  lv_disp_drv_register(&s_dispDrv);
  s_lcd->attachDrawBitmapFinishCallback(onDrawDone, &s_dispDrv);

  lv_indev_drv_init(&s_touchDrv);
  s_touchDrv.type = LV_INDEV_TYPE_POINTER;
  s_touchDrv.read_cb = touchReadCb;
  s_touchDrv.user_data = s_touch;
  // LVGL defaults (50 px / 3 px-per-sample) let short swipes fall through as taps.
  s_touchDrv.gesture_limit = 30;
  s_touchDrv.gesture_min_velocity = 2;
  lv_indev_drv_register(&s_touchDrv);
}

void initKnob() {
  // After a deep-sleep wake the wake pins are still routed to the RTC mux; hand them back
  // to the digital GPIO matrix or normal reads return garbage.
  for (int pin : {PIN_ENC_A, PIN_ENC_B, PIN_TP_INT}) {
    if (rtc_gpio_is_valid_gpio(static_cast<gpio_num_t>(pin))) rtc_gpio_deinit(static_cast<gpio_num_t>(pin));
  }
  for (KnobChan &c : s_chans) {
    pinMode(c.pin, INPUT_PULLUP);
    // A pin already low at boot is the twist that woke us from deep sleep: don't count it.
    if (gpio_get_level(c.pin) == 0) {
      c.armed = false;
      c.highRun = 0;
    }
  }
  const esp_timer_create_args_t args = {.callback = knobSample,
                                        .arg = nullptr,
                                        .dispatch_method = ESP_TIMER_TASK,
                                        .name = "knob",
                                        .skip_unhandled_events = true};
  ESP_ERROR_CHECK(esp_timer_create(&args, &s_knobTimer));
  ESP_ERROR_CHECK(esp_timer_start_periodic(s_knobTimer, 1000));  // 1 kHz
}

}  // namespace

void begin() {
  initBacklight();
  initTouch();
  initLcd();
  initLvgl();
  initKnob();
  initHaptics();
  analogSetPinAttenuation(PIN_BATT_ADC, ADC_11db);
  s_backlight->on();
  s_backlight->setBrightness(100);
}

void deepSleep(bool touchWake) {
  // Display. The backlight is LEDC PWM on GPIO47, a digital (non-RTC) pin: in deep sleep the
  // S3 releases it and it floats, which let the backlight come back on over a frozen frame.
  // So take it off LEDC, drive it low and latch it with the deep-sleep pad hold.
  if (s_backlight) {
    s_backlight->setBrightness(0);
    s_backlight->off();
  }
  const auto bl = static_cast<gpio_num_t>(PIN_LCD_BL);
  gpio_reset_pin(bl);
  gpio_set_direction(bl, GPIO_MODE_OUTPUT);
  gpio_set_level(bl, 0);
  gpio_hold_en(bl);
  gpio_deep_sleep_hold_en();

  // Panel: the driver has no disp_sleep for the ST77916, so send DISPOFF (0x28) and SLPIN
  // (0x10) ourselves, in the QSPI command framing the driver uses (opcode 0x02, cmd << 8).
  if (s_lcdBus) {
    auto io = s_lcdBus->getControlPanelHandle();
    esp_lcd_panel_io_tx_param(io, (0x02 << 24) | (0x28 << 8), nullptr, 0);
    esp_lcd_panel_io_tx_param(io, (0x02 << 24) | (0x10 << 8), nullptr, 0);
    delay(10);  // SLPIN needs 5 ms before anything else
  }

  // Haptics: standby (MODE bit 6).
  if (s_haptics) drvWrite(0x01, 0x40);

  // Touch (CST816S @ 0x15): deep sleep unless it's a wake source. Reset on boot wakes it.
  if (!touchWake) {
    const uint8_t cmd[2] = {0xE5, 0x03};
    i2c_master_write_to_device(kI2cPort, 0x15, cmd, 2, pdMS_TO_TICKS(20));
  }

  if (s_knobTimer) esp_timer_stop(s_knobTimer);

  // Wake on either knob pin going low (both are RTC GPIOs with 10 k external pull-ups),
  // plus the touch interrupt if enabled. A pin that is already low would wake us at once
  // (and loop): give it a moment to release, and leave out any pin that stays low.
  uint64_t mask = 0;
  for (int pin : {PIN_ENC_A, PIN_ENC_B}) {
    const uint32_t t0 = millis();
    while (digitalRead(pin) == LOW && millis() - t0 < 1000) delay(5);
    if (digitalRead(pin) == LOW) Serial.printf("[hal] knob pin %d stuck low; not a wake source\n", pin);
    else mask |= 1ULL << pin;
  }
  if (touchWake) mask |= 1ULL << PIN_TP_INT;
  if (mask == 0) mask = 1ULL << PIN_ENC_A;  // never sleep with no way to wake
  for (int pin : {PIN_ENC_A, PIN_ENC_B, PIN_TP_INT}) {
    if (!(mask & (1ULL << pin))) continue;
    rtc_gpio_init(static_cast<gpio_num_t>(pin));
    rtc_gpio_set_direction(static_cast<gpio_num_t>(pin), RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pullup_en(static_cast<gpio_num_t>(pin));
    rtc_gpio_pulldown_dis(static_cast<gpio_num_t>(pin));
  }
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);  // keep RTC pull-ups alive
  esp_sleep_enable_ext1_wakeup(mask, ESP_EXT1_WAKEUP_ANY_LOW);

  Serial.printf("[hal] deep sleep (wake mask 0x%llx)\n", mask);
  Serial.flush();
  delay(20);
  esp_deep_sleep_start();
}

void setBacklight(uint8_t percent) {
  if (s_backlight) s_backlight->setBrightness(percent > 100 ? 100 : percent);
}

int32_t takeKnobDelta() { return s_knobDelta.exchange(0); }

bool hasHaptics() { return s_haptics; }

uint32_t centreHoldMs(int radiusPx) {
  if (!s_touchDown || s_touchMoved) return 0;
  const int dx = s_touchX0 - LCD_WIDTH / 2, dy = s_touchY0 - LCD_HEIGHT / 2;
  if (dx * dx + dy * dy > radiusPx * radiusPx) return 0;
  return millis() - s_touchStartMs;
}

void service() {}

void haptic(Haptic h) {
  if (!s_haptics) return;
  // {drive amplitude (signed, 0x7F = full), duration}. Tune here.
  struct Pulse { uint8_t amp; uint32_t us; };
  static constexpr Pulse kPulses[] = {
      // The LRA needs ~3 resonant cycles (~15 ms) to build up; shorter pulses weren't felt.
      {0x7F, 15000},  // Tick: page / mode change, end stop
      {0x7F, 18000},  // Click: light on/off
      {0x7F, 24000},  // Bump: turned off by dialling to 0
  };
  const Pulse &p = kPulses[static_cast<int>(h)];
  esp_timer_stop(s_hapticOff);
  drvWrite(0x02, p.amp);
  esp_timer_start_once(s_hapticOff, p.us);
}

float supplyVolts() {
  // 10 k / 10 k divider from the 5 V rail (= battery when unplugged) to GPIO1.
  uint32_t mv = 0;
  for (int i = 0; i < 8; ++i) mv += analogReadMilliVolts(PIN_BATT_ADC);
  return (mv / 8) * 2 / 1000.0f;
}

}  // namespace hal
