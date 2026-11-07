#include "ui.h"

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_attr.h>
#include <lvgl.h>

#include "battery.h"
#include "config.h"
#include "hal.h"
#include "lights.h"

namespace ui {
namespace {

// Knob step sizes per detent when turning slowly. Resolution is fixed by the mechanical
// detents, so turning quickly multiplies the step (see knobMultiplier) to still sweep a full
// range in one wrist turn.
constexpr int kPctPerDetent = 2;
constexpr int kKelvinPerDetent = 100;
constexpr int kHuePerDetent = 5;

// After waking, ignore swipes briefly so the waking touch doesn't also change page.
constexpr uint32_t kWakeGestureGuardMs = 600;

// "All lights" button: a tap only counts if the finger barely moved and lifted quickly, so
// a swipe that falls short of the gesture threshold can't press it.
constexpr int kTapMaxMovePx = 14;
constexpr uint32_t kTapMaxMs = 450;

enum class Mode : uint8_t { Brightness, Temperature, Colour, Count };
const char *const kModeNames[] = {"Brightness", "Temperature", "Colour"};

// RTC_DATA_ATTR: kept through deep sleep so the knob wakes on the same page.
RTC_DATA_ATTR int s_page = 0;  // 0 = all lights, n = bulb n-1
RTC_DATA_ATTR Mode s_mode = Mode::Brightness;
uint32_t s_seenRev = 0;

// Display sleep state.
bool s_screenOn = true;
uint32_t s_lastKnobMs = 0;
uint32_t s_lastInputMs = 0;
uint32_t s_wakeMs = 0;
uint32_t s_lastTouchInactive = 0;

// Bulbs the "All lights" dial switches back on: the set that was on when everything was
// last switched off from that page. Persisted (NVS) so it survives reboots / deep sleep.
uint8_t s_allMask = 0;
Preferences s_prefs;

// Remember the last temperature / hue per bulb so switching modes starts somewhere sensible.
RTC_DATA_ATTR uint16_t s_lastKelvin[lights::kMaxBulbs];
RTC_DATA_ATTR uint16_t s_lastHue[lights::kMaxBulbs];

bool s_wantSleep = false;
lv_obj_t *s_allBtn, *s_allIcon;
lv_point_t s_pressPt;
uint32_t s_pressMs = 0;
bool s_gestureSincePress = false;

lv_obj_t *s_batt;
lv_obj_t *s_center, *s_valueRow, *s_unit;
lv_obj_t *s_tempRing;
// What the temperature ring draws (set in renderBulb, read by drawTempRing).
uint16_t s_ringK = 4000, s_ringMin = 2500, s_ringMax = 6500;
bool s_ringLit = false;
lv_obj_t *s_arc, *s_wheel, *s_value, *s_status, *s_name, *s_modeLabel, *s_pageDots, *s_modeDots;

const lv_color_t kOffColor = lv_color_hex(0x404040);
const lv_color_t kTrackColor = lv_color_hex(0x1A1A1A);
const lv_color_t kDimText = lv_color_hex(0x9A9A9A);
const lv_color_t kAllOnColor = lv_color_hex(0xFFB347);

int pageCount() { return lights::count() + 1; }
int bulbIndex() { return s_page - 1; }
bool online() { return lights::netStatus() == lights::NetStatus::Online; }
// Input is accepted as soon as we have (possibly cached) bulb state, even while Wi-Fi is
// still reconnecting after a deep-sleep wake; changes queue and go out once connected.
bool networkConfigured() { return lights::netStatus() != lights::NetStatus::NotConfigured; }

// Rough Kelvin -> display colour (warm amber .. cool white).
lv_color_t kelvinColor(uint16_t k, uint16_t lo = 2500, uint16_t hi = 6500) {
  if (k < lo) k = lo;
  if (k > hi) k = hi;
  const uint8_t t = static_cast<uint8_t>((k - lo) * 255 / (hi - lo ? hi - lo : 1));
  return lv_color_mix(lv_color_hex(0xDDEBFF), lv_color_hex(0xFFA040), t);
}

lv_color_t bulbColor(const lights::Bulb &b) {
  if (b.colorTemp) return kelvinColor(b.colorTemp, b.ctMin, b.ctMax);
  return lv_color_hsv_to_rgb(b.hue, b.saturation < 15 ? 15 : b.saturation, 100);
}

uint16_t currentKelvin(const lights::Bulb &b) {
  if (b.colorTemp) return b.colorTemp;
  uint16_t k = s_lastKelvin[bulbIndex()];
  return k ? k : (b.ctMin + b.ctMax) / 2;
}

uint16_t currentHue(const lights::Bulb &b) {
  return b.colorTemp == 0 ? b.hue : s_lastHue[bulbIndex()];
}

void setDots(lv_obj_t *row, int active) {
  for (uint32_t i = 0; i < lv_obj_get_child_cnt(row); ++i) {
    lv_obj_set_style_bg_color(lv_obj_get_child(row, i),
                              static_cast<int>(i) == active ? lv_color_white() : kOffColor, 0);
  }
}

lv_obj_t *makeDots(lv_obj_t *parent, int n, lv_flex_flow_t flow) {
  lv_obj_t *row = lv_obj_create(parent);
  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(row, flow);
  lv_obj_set_style_pad_gap(row, 8, 0);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
  for (int i = 0; i < n; ++i) {
    lv_obj_t *d = lv_obj_create(row);
    lv_obj_remove_style_all(d);
    lv_obj_set_size(d, 8, 8);
    lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
  }
  return row;
}

void show(lv_obj_t *o, bool visible) {
  if (visible) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

// Status line shared by all pages when the network isn't usable. Returns nullptr when fine.
const char *netProblem(const lights::Bulb *b) {
  switch (lights::netStatus()) {
    case lights::NetStatus::NotConfigured: return "Edit secrets.h";
    case lights::NetStatus::Connecting: return "Connecting Wi-Fi...";
    default: break;
  }
  if (b && !b->reachable) {
    if (!b->known && b->error[0] == '\0') return "Connecting...";
    return b->error[0] ? b->error : "Offline";
  }
  return nullptr;
}

void renderAll() {
  int on = 0, reachable = 0;
  for (int i = 0; i < lights::count(); ++i) {
    const lights::Bulb b = lights::get(i);
    if (!b.known) continue;
    ++reachable;
    if (b.on) ++on;
  }
  const bool lit = networkConfigured() && on > 0;

  lv_label_set_text(s_name, "All lights");
  lv_label_set_text(s_modeLabel, "Tap to switch all");
  show(s_arc, false);
  show(s_wheel, false);
  show(s_tempRing, false);
  show(s_valueRow, false);
  show(s_modeDots, false);
  show(s_allBtn, true);
  lv_obj_align(s_status, LV_ALIGN_CENTER, 0, 28);

  lv_obj_set_style_border_color(s_allBtn, lit ? kAllOnColor : kOffColor, 0);
  lv_obj_set_style_text_color(s_allIcon, lit ? kAllOnColor : kOffColor, 0);

  const char *problem = netProblem(nullptr);
  if (problem) lv_label_set_text(s_status, problem);
  else if (reachable == 0) lv_label_set_text(s_status, "No lights reachable");
  else if (on == 0) lv_label_set_text(s_status, "All off");
  else lv_label_set_text_fmt(s_status, "%d of %d on", on, lights::count());
}

void renderBulb() {
  const lights::Bulb b = lights::get(bulbIndex());
  const bool live = networkConfigured() && b.known;
  const bool lit = live && b.on;

  lv_label_set_text(s_name, b.name);
  lv_label_set_text(s_modeLabel, kModeNames[static_cast<int>(s_mode)]);
  show(s_modeDots, true);
  show(s_allBtn, false);
  setDots(s_modeDots, static_cast<int>(s_mode));

  const bool colour = s_mode == Mode::Colour;
  const bool temp = s_mode == Mode::Temperature;
  show(s_arc, s_mode == Mode::Brightness);
  show(s_tempRing, temp);
  show(s_wheel, colour);
  show(s_valueRow, !colour);  // the hue ring already shows the colour
  lv_obj_align(s_status, LV_ALIGN_CENTER, 0, colour ? 0 : 28);

  switch (s_mode) {
    case Mode::Brightness: {
      const uint8_t pct = lit ? b.brightness : 0;
      lv_arc_set_range(s_arc, 0, 100);
      lv_arc_set_value(s_arc, pct);
      lv_obj_set_style_arc_color(s_arc, bulbColor(b), LV_PART_INDICATOR);
      lv_label_set_text_fmt(s_value, "%u", pct);
      lv_label_set_text(s_unit, "%");
      break;
    }
    case Mode::Temperature: {
      const uint16_t k = currentKelvin(b);
      s_ringK = k;
      s_ringMin = b.ctMin;
      s_ringMax = b.ctMax;
      s_ringLit = lit;
      lv_obj_invalidate(s_tempRing);
      lv_label_set_text_fmt(s_value, "%u", k);
      lv_label_set_text(s_unit, "K");
      break;
    }
    case Mode::Colour: {
      const uint16_t hue = currentHue(b);
      lv_colorwheel_set_hsv(s_wheel, lv_color_hsv_t{hue, 100, 100});
      lv_obj_invalidate(s_wheel);  // the arrow is drawn outside the knob area LVGL tracks
      break;
    }
    default: break;
  }
  if (!lit) lv_obj_set_style_arc_color(s_arc, kOffColor, LV_PART_INDICATOR);
  lv_obj_set_style_text_color(s_value, lit ? lv_color_white() : kOffColor, 0);
  lv_obj_set_style_text_color(s_unit, lit ? kDimText : kOffColor, 0);

  const char *problem = netProblem(&b);
  lv_label_set_text(s_status, problem ? problem : (b.on ? "ON" : "OFF"));
}

void render() {
  setDots(s_pageDots, s_page);
  if (s_page == 0) renderAll();
  else renderBulb();
}

// ---- Battery indicator -------------------------------------------------------
// Voltage, USB detection and the V -> % curve live in battery.cpp. On USB the supply sense
// reads VBUS, not the cell, and the charger's status pin isn't wired to the S3, so we can
// only say "on USB" there.
uint32_t s_battMs = 0;

void updateBattery(bool force) {
  if (!force && millis() - s_battMs < 5000) return;
  s_battMs = millis();
  if (battery::onUsb()) {
    lv_label_set_text(s_batt, LV_SYMBOL_USB " " LV_SYMBOL_CHARGE);
    lv_obj_set_style_text_color(s_batt, kDimText, 0);
    return;
  }
  const int pct = battery::percent();
  const char *sym = pct > 80 ? LV_SYMBOL_BATTERY_FULL
                  : pct > 55 ? LV_SYMBOL_BATTERY_3
                  : pct > 30 ? LV_SYMBOL_BATTERY_2
                  : pct > 10 ? LV_SYMBOL_BATTERY_1
                             : LV_SYMBOL_BATTERY_EMPTY;
  lv_label_set_text_fmt(s_batt, "%s %d%%", sym, pct);
  lv_obj_set_style_text_color(s_batt, pct <= 10 ? lv_color_hex(0xFF5040) : kDimText, 0);
}

// ---- Secret menu: debug info + battery calibration ----------------------------
// Open/close by holding a finger still on the screen centre for 10 s. Lives on the top
// layer, so the screen's swipe handler never sees touches on it.
constexpr uint32_t kMenuHoldMs = 10000;
constexpr int kMenuHoldRadiusPx = 90;
lv_obj_t *s_menu = nullptr, *s_menuInfo, *s_menuCal, *s_menuBtnLabel;
bool s_menuOpen = false;
bool s_holdLatched = false;  // one toggle per hold
uint32_t s_menuRefreshMs = 0;

void menuRefresh() {
  // Debug info.
  char buf[640];
  int n = snprintf(buf, sizeof(buf), "v" FW_VERSION "  #" GIT_HASH "\n");
  if (WiFi.status() == WL_CONNECTED) {
    n += snprintf(buf + n, sizeof(buf) - n, "Wi-Fi: %s (%d dBm)\nIP %s\n", WiFi.SSID().c_str(), WiFi.RSSI(),
                  WiFi.localIP().toString().c_str());
  } else {
    n += snprintf(buf + n, sizeof(buf) - n, "Wi-Fi: not connected\n");
  }
  for (int i = 0; i < lights::count() && n < static_cast<int>(sizeof(buf)) - 64; ++i) {
    const lights::Bulb b = lights::get(i);
    n += snprintf(buf + n, sizeof(buf) - n, "%s %s  %s\n", b.reachable ? LV_SYMBOL_OK : LV_SYMBOL_CLOSE, b.name,
                  lights::ip(i));
  }
  snprintf(buf + n, sizeof(buf) - n, "Battery %.3f V%s, %s curve", battery::volts(), battery::onUsb() ? " (USB)" : "",
           battery::calibrated() ? "calibrated" : "generic");
  lv_label_set_text(s_menuInfo, buf);

  // Calibration.
  const char *last = battery::calLastResult();
  switch (battery::calState()) {
    case battery::CalState::Idle:
      lv_label_set_text_fmt(s_menuCal,
                            "Battery calibration: charge fully, press Start, then unplug. "
                            "Runs flat out until the battery dies (a few hours). "
                            "The new curve is applied on the next boot.%s%s",
                            last[0] ? "\nLast run: " : "", last);
      lv_label_set_text(s_menuBtnLabel, "Start calibration");
      break;
    case battery::CalState::Armed:
      lv_label_set_text(s_menuCal, "Armed. Unplug USB to start logging.");
      lv_label_set_text(s_menuBtnLabel, "Cancel");
      break;
    case battery::CalState::Running: {
      const uint32_t t = battery::calElapsedS();
      lv_label_set_text_fmt(s_menuCal, "Logging %lu:%02lu:%02lu, %lu samples.\nLeave it running until it switches off.",
                            static_cast<unsigned long>(t / 3600), static_cast<unsigned long>((t / 60) % 60),
                            static_cast<unsigned long>(t % 60), static_cast<unsigned long>(battery::calSamples()));
      lv_label_set_text(s_menuBtnLabel, "Stop");
      break;
    }
  }
}

void onMenuButton(lv_event_t *) {
  if (battery::calState() == battery::CalState::Idle) battery::calStart();
  else battery::calStop();
  hal::haptic(hal::Haptic::Click);
  menuRefresh();
}

void onMenuClose(lv_event_t *);

void buildMenu() {
  s_menu = lv_obj_create(lv_layer_top());
  lv_obj_set_size(s_menu, LV_PCT(100), LV_PCT(100));
  lv_obj_center(s_menu);
  lv_obj_set_style_bg_color(s_menu, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(s_menu, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(s_menu, 0, 0);
  lv_obj_set_style_radius(s_menu, 0, 0);
  lv_obj_set_style_pad_hor(s_menu, 48, 0);  // keep text inside the round glass
  lv_obj_set_style_pad_ver(s_menu, 40, 0);
  lv_obj_set_flex_flow(s_menu, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(s_menu, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(s_menu, 10, 0);
  lv_obj_set_scroll_dir(s_menu, LV_DIR_VER);

  lv_obj_t *title = lv_label_create(s_menu);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_18, 0);
  lv_obj_set_style_text_color(title, lv_color_white(), 0);
  lv_label_set_text(title, "Debug");

  auto text = [](lv_obj_t *parent) {
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_width(l, LV_PCT(100));
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0xC8C8C8), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    return l;
  };
  s_menuInfo = text(s_menu);
  s_menuCal = text(s_menu);

  auto button = [](lv_obj_t *parent, lv_event_cb_t cb, lv_obj_t **labelOut) {
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x2A2A2A), 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_16, 0);
    if (labelOut) *labelOut = l;
    return l;
  };
  button(s_menu, onMenuButton, &s_menuBtnLabel);
  lv_label_set_text(button(s_menu, onMenuClose, nullptr), "Close");
}

void openMenu(bool open) {
  s_menuOpen = open;
  if (open) {
    if (!s_menu) buildMenu();
    lv_obj_clear_flag(s_menu, LV_OBJ_FLAG_HIDDEN);
    lv_obj_scroll_to_y(s_menu, 0, LV_ANIM_OFF);
    menuRefresh();
    s_menuRefreshMs = millis();
  } else if (s_menu) {
    lv_obj_add_flag(s_menu, LV_OBJ_FLAG_HIDDEN);
  }
  hal::haptic(hal::Haptic::Bump);
}

void onMenuClose(lv_event_t *) { openMenu(false); }

// Black arrowhead sitting on a ring, pointing inward. (ux, uy) is the unit direction from the
// centre in LV_TRIGO fixed point.
void drawArrow(lv_draw_ctx_t *ctx, int cx, int cy, int rOuter, int ring, int32_t ux, int32_t uy) {
  auto at = [&](int r, int side) {  // radius r along (ux,uy), `side` px perpendicular
    return lv_point_t{static_cast<lv_coord_t>(cx + ((r * ux + side * uy) >> LV_TRIGO_SHIFT)),
                      static_cast<lv_coord_t>(cy + ((r * uy - side * ux) >> LV_TRIGO_SHIFT))};
  };
  const lv_point_t tri[3] = {at(rOuter - ring - 4, 0), at(rOuter - 1, -11), at(rOuter - 1, 11)};
  lv_draw_rect_dsc_t dsc;
  lv_draw_rect_dsc_init(&dsc);
  dsc.bg_color = lv_color_black();
  dsc.bg_opa = LV_OPA_COVER;
  lv_draw_polygon(ctx, &dsc, tri, 3);
}

// Hue ring: replaces the colorwheel's round knob, which overlapped the labels.
void drawHueArrow(lv_event_t *e) {
  lv_obj_t *obj = lv_event_get_target(e);
  const lv_area_t &c = obj->coords;
  const uint16_t a = lv_colorwheel_get_hsv(obj).h % 360;
  // lv_colorwheel's convention: x = sin(hue), y = cos(hue).
  drawArrow(lv_event_get_draw_ctx(e), (c.x1 + c.x2) / 2, (c.y1 + c.y2) / 2, lv_area_get_width(&c) / 2,
            lv_obj_get_style_arc_width(obj, LV_PART_MAIN), lv_trigo_sin(a), lv_trigo_cos(a));
}

// Temperature ring: warm -> cool gradient over the same 270 degree sweep as the brightness
// arc (gap at the bottom), with the arrow at the current Kelvin.
constexpr int kRingWidth = 22;
constexpr int kSweepStart = 135, kSweep = 270;  // LVGL angles: 0 = 3 o'clock, clockwise

void drawTempRing(lv_event_t *e) {
  lv_obj_t *obj = lv_event_get_target(e);
  lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(e);
  const lv_area_t &c = obj->coords;
  const lv_point_t ctr{static_cast<lv_coord_t>((c.x1 + c.x2) / 2), static_cast<lv_coord_t>((c.y1 + c.y2) / 2)};
  const int r = lv_area_get_width(&c) / 2;
  const int span = s_ringMax > s_ringMin ? s_ringMax - s_ringMin : 1;

  lv_draw_arc_dsc_t arc;
  lv_draw_arc_dsc_init(&arc);
  arc.width = kRingWidth;
  arc.opa = s_ringLit ? LV_OPA_COVER : LV_OPA_40;
  constexpr int kStep = 3;
  for (int off = 0; off < kSweep; off += kStep) {
    arc.color = kelvinColor(s_ringMin + span * (off + kStep / 2) / kSweep, s_ringMin, s_ringMax);
    const int end = off + kStep + 1 > kSweep ? kSweep : off + kStep + 1;  // +1 overlap: no seams
    lv_draw_arc(ctx, &arc, &ctr, r, (kSweepStart + off) % 360, (kSweepStart + end) % 360);
  }
  const int ang = kSweepStart + kSweep * (s_ringK - s_ringMin) / span;
  drawArrow(ctx, ctr.x, ctr.y, r, kRingWidth, lv_trigo_cos(ang), lv_trigo_sin(ang));
}

// ---- Input -----------------------------------------------------------------

// "All lights" dial: right = restore the remembered set, left = remember what's on, then
// switch everything off.
void allLights(bool on) {
  if (on) {
    uint8_t mask = s_allMask;
    if (mask == 0) mask = (1u << lights::count()) - 1;  // nothing remembered: all of them
    bool changed = false;
    for (int i = 0; i < lights::count(); ++i) {
      const lights::Bulb b = lights::get(i);
      if ((mask & (1u << i)) && b.known && !b.on) {
        lights::setPower(i, true);
        changed = true;
      }
    }
    if (changed) hal::haptic(hal::Haptic::Click);
    return;
  }
  uint8_t mask = 0;
  for (int i = 0; i < lights::count(); ++i) {
    const lights::Bulb b = lights::get(i);
    if (b.known && b.on) mask |= 1u << i;
  }
  if (mask == 0) return;  // already all off: keep the previous memory
  if (mask != s_allMask) {
    s_allMask = mask;
    s_prefs.putUChar("allMask", mask);
  }
  lights::setPower(lights::kAll, false);
  hal::haptic(hal::Haptic::Bump);
}

void onAllPressed(lv_event_t *) {
  lv_indev_get_point(lv_indev_get_act(), &s_pressPt);
  s_pressMs = millis();
  s_gestureSincePress = false;
}

void onAllClicked(lv_event_t *) {
  if (!s_screenOn || millis() - s_wakeMs < kWakeGestureGuardMs) return;  // the waking touch
  lv_point_t p;
  lv_indev_get_point(lv_indev_get_act(), &p);
  const int dx = p.x - s_pressPt.x, dy = p.y - s_pressPt.y;
  if (s_gestureSincePress || millis() - s_pressMs > kTapMaxMs ||
      dx * dx + dy * dy > kTapMaxMovePx * kTapMaxMovePx || !networkConfigured()) {
    return;
  }
  bool anyOn = false;
  for (int i = 0; i < lights::count(); ++i) {
    const lights::Bulb b = lights::get(i);
    anyOn |= b.known && b.on;
  }
  allLights(!anyOn);
}

void onGesture(lv_event_t *) {
  s_gestureSincePress = true;
  if (!s_screenOn || millis() - s_wakeMs < kWakeGestureGuardMs) return;  // waking touch
  const lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());
  if (dir == LV_DIR_LEFT || dir == LV_DIR_RIGHT) {
    const int n = pageCount();
    s_page = (s_page + (dir == LV_DIR_LEFT ? 1 : n - 1)) % n;
    s_mode = Mode::Brightness;
  } else if (s_page != 0 && (dir == LV_DIR_TOP || dir == LV_DIR_BOTTOM)) {
    const int n = static_cast<int>(Mode::Count);
    s_mode = static_cast<Mode>((static_cast<int>(s_mode) + (dir == LV_DIR_TOP ? 1 : n - 1)) % n);
  } else {
    return;
  }
  hal::haptic(hal::Haptic::Tick);
  render();
}

// Gap between detents -> step multiplier.
int knobMultiplier(int32_t detents) {
  static uint32_t lastMs = 0;
  const uint32_t now = millis();
  const uint32_t gap = now - lastMs;
  lastMs = now;
  if (detents > 1 || detents < -1 || gap < 45) return 4;  // spinning
  if (gap < 110) return 2;                               // brisk
  return 1;                                              // deliberate, single steps
}

void applyKnob(int32_t d) {
  if (!networkConfigured()) return;
  if (s_page == 0) {  // "All lights": on/off only
    allLights(d > 0);
    return;
  }
  const int idx = bulbIndex();
  const lights::Bulb b = lights::get(idx);
  if (!b.known) return;
  d *= knobMultiplier(d);

  switch (s_mode) {
    case Mode::Brightness: {
      const int cur = b.on ? b.brightness : 0;
      int v = cur + d * kPctPerDetent;
      if (v <= 0) {
        if (b.on) {
          lights::setPower(idx, false);
          hal::haptic(hal::Haptic::Bump);
        }
        return;
      }
      if (v >= 100) {
        if (cur >= 100) {
          hal::haptic(hal::Haptic::Tick);  // already at the end stop
          return;
        }
        v = 100;
      }
      if (!b.on) hal::haptic(hal::Haptic::Click);
      lights::setBrightness(idx, static_cast<uint8_t>(v));
      break;
    }
    case Mode::Temperature: {
      const int cur = currentKelvin(b);
      int k = cur + d * kKelvinPerDetent;
      if (k < b.ctMin) k = b.ctMin;
      if (k > b.ctMax) k = b.ctMax;
      if (k == cur && b.colorTemp) {
        hal::haptic(hal::Haptic::Tick);
        return;
      }
      s_lastKelvin[idx] = static_cast<uint16_t>(k);
      lights::setColorTemp(idx, static_cast<uint16_t>(k));
      break;
    }
    case Mode::Colour: {
      int hue = (static_cast<int>(currentHue(b)) + d * kHuePerDetent) % 360;
      if (hue < 0) hue += 360;
      s_lastHue[idx] = static_cast<uint16_t>(hue);
      lights::setHueSat(idx, static_cast<uint16_t>(hue), 100);
      break;
    }
    default: break;
  }
}

}  // namespace

void begin() {
  lv_obj_t *scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(scr, onGesture, LV_EVENT_GESTURE, nullptr);

  // Display-only dials: the knob is the input, so they never steal swipes.
  s_arc = lv_arc_create(scr);
  lv_obj_set_size(s_arc, 340, 340);
  lv_obj_center(s_arc);
  lv_arc_set_rotation(s_arc, 135);
  lv_arc_set_bg_angles(s_arc, 0, 270);
  lv_obj_clear_flag(s_arc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_style(s_arc, nullptr, LV_PART_KNOB);
  lv_obj_set_style_arc_width(s_arc, 22, LV_PART_MAIN);
  lv_obj_set_style_arc_width(s_arc, 22, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(s_arc, kTrackColor, LV_PART_MAIN);

  s_wheel = lv_colorwheel_create(scr, true);
  lv_obj_set_size(s_wheel, 340, 340);
  lv_obj_center(s_wheel);
  lv_colorwheel_set_mode_fixed(s_wheel, true);
  lv_obj_set_style_arc_width(s_wheel, 22, LV_PART_MAIN);
  lv_obj_clear_flag(s_wheel, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_opa(s_wheel, LV_OPA_TRANSP, LV_PART_KNOB);
  lv_obj_add_event_cb(s_wheel, drawHueArrow, LV_EVENT_DRAW_POST, nullptr);

  s_tempRing = lv_obj_create(scr);
  lv_obj_remove_style_all(s_tempRing);
  lv_obj_set_size(s_tempRing, 340, 340);
  lv_obj_center(s_tempRing);
  lv_obj_clear_flag(s_tempRing, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(s_tempRing, drawTempRing, LV_EVENT_DRAW_MAIN, nullptr);

  s_name = lv_label_create(scr);
  lv_obj_set_style_text_font(s_name, &lv_font_montserrat_22, 0);
  lv_obj_set_style_text_color(s_name, lv_color_white(), 0);
  lv_obj_set_width(s_name, 220);
  lv_obj_set_style_text_align(s_name, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(s_name, LV_LABEL_LONG_DOT);
  lv_obj_align(s_name, LV_ALIGN_TOP_MID, 0, 66);

  s_modeLabel = lv_label_create(scr);
  lv_obj_set_style_text_font(s_modeLabel, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(s_modeLabel, kDimText, 0);
  lv_obj_align(s_modeLabel, LV_ALIGN_TOP_MID, 0, 94);

  // Centre readout (display only; the knob is the only control).
  s_center = lv_obj_create(scr);
  lv_obj_remove_style_all(s_center);
  lv_obj_set_size(s_center, 112, 112);
  lv_obj_align(s_center, LV_ALIGN_CENTER, 0, 16);
  lv_obj_clear_flag(s_center, LV_OBJ_FLAG_CLICKABLE);

  // Number + small unit ("72" "%", "4000" "K").
  s_valueRow = lv_obj_create(s_center);
  lv_obj_remove_style_all(s_valueRow);
  lv_obj_set_size(s_valueRow, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(s_valueRow, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(s_valueRow, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
  lv_obj_set_style_pad_column(s_valueRow, 2, 0);
  lv_obj_clear_flag(s_valueRow, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_align(s_valueRow, LV_ALIGN_CENTER, 0, -8);

  s_value = lv_label_create(s_valueRow);
  lv_obj_set_style_text_font(s_value, &lv_font_montserrat_32, 0);

  s_unit = lv_label_create(s_valueRow);
  lv_obj_set_style_text_font(s_unit, &lv_font_montserrat_16, 0);
  lv_obj_set_style_pad_bottom(s_unit, 4, 0);

  s_status = lv_label_create(s_center);
  lv_obj_set_style_text_font(s_status, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(s_status, kDimText, 0);
  lv_obj_set_width(s_status, 92);
  lv_obj_set_style_text_align(s_status, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(s_status, LV_LABEL_LONG_DOT);
  lv_obj_align(s_status, LV_ALIGN_CENTER, 0, 28);

  // "All lights" power button: small, a little above centre, status text below it.
  s_allBtn = lv_btn_create(scr);
  lv_obj_set_size(s_allBtn, 76, 76);
  lv_obj_align(s_allBtn, LV_ALIGN_CENTER, 0, -14);
  lv_obj_set_style_radius(s_allBtn, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(s_allBtn, lv_color_hex(0x121212), 0);
  lv_obj_set_style_bg_color(s_allBtn, lv_color_hex(0x2A2A2A), LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(s_allBtn, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(s_allBtn, 2, 0);
  lv_obj_set_style_shadow_width(s_allBtn, 0, 0);
  lv_obj_add_event_cb(s_allBtn, onAllPressed, LV_EVENT_PRESSED, nullptr);
  lv_obj_add_event_cb(s_allBtn, onAllClicked, LV_EVENT_CLICKED, nullptr);
  s_allIcon = lv_label_create(s_allBtn);
  lv_obj_set_style_text_font(s_allIcon, &lv_font_montserrat_32, 0);
  lv_label_set_text(s_allIcon, LV_SYMBOL_POWER);
  lv_obj_center(s_allIcon);

  s_pageDots = makeDots(scr, pageCount(), LV_FLEX_FLOW_ROW);
  lv_obj_align(s_pageDots, LV_ALIGN_BOTTOM_MID, 0, -40);

  s_modeDots = makeDots(scr, static_cast<int>(Mode::Count), LV_FLEX_FLOW_COLUMN);
  lv_obj_align(s_modeDots, LV_ALIGN_RIGHT_MID, -40, 0);

  // Battery sits in the gap at the bottom of the dial, above the page dots.
  s_batt = lv_label_create(scr);
  lv_obj_set_style_text_font(s_batt, &lv_font_montserrat_14, 0);
  lv_obj_align(s_batt, LV_ALIGN_BOTTOM_MID, 0, -58);
  updateBattery(true);

  s_prefs.begin("knob", false);
  s_allMask = s_prefs.getUChar("allMask", 0);
  s_lastKnobMs = s_lastInputMs = s_wakeMs = millis();
  if (s_page >= pageCount()) s_page = 0;

  render();
}

namespace {
void setScreen(bool on) {
  if (on == s_screenOn) return;
  s_screenOn = on;
  hal::setBacklight(on ? 100 : 0);
  lights::setPowerSave(!on);  // modem sleep while dark; full speed while in use
  if (on) s_wakeMs = millis();
}

bool onUsb() { return battery::onUsb(); }
}  // namespace

bool wantsDeepSleep() { return s_wantSleep; }

void tick(int32_t knobDelta) {
  // Display sleep. Touch activity comes from LVGL's inactivity timer; knob from here.
  const uint32_t touchIdle = lv_disp_get_inactive_time(nullptr);
  const bool touched = touchIdle < s_lastTouchInactive;
  s_lastTouchInactive = touchIdle;
  if (knobDelta != 0) s_lastKnobMs = millis();
  if (touched || knobDelta != 0) s_lastInputMs = millis();
  const uint32_t idleMs = millis() - s_lastInputMs;
  // Calibrating = a constant, high load until the battery dies: never dim or sleep.
  const bool calibrating = battery::calState() != battery::CalState::Idle;
  if (!s_screenOn) {
    if (touched || knobDelta != 0 || calibrating) setScreen(true);  // the waking input does nothing else
    knobDelta = 0;
  } else if (idleMs > SCREEN_TIMEOUT_MS && !calibrating && !s_menuOpen) {
    setScreen(false);
  }
  // Deep sleep once idle long enough and nothing is still waiting to be sent to a bulb.
  if (idleMs > DEEP_SLEEP_AFTER_MS && !calibrating && !s_menuOpen && (DEEP_SLEEP_ON_USB || !onUsb()) &&
      lights::idle()) {
    s_wantSleep = true;
  }

  // Secret menu: hold the centre still for 10 s (toggles; one toggle per hold).
  const uint32_t held = hal::centreHoldMs(kMenuHoldRadiusPx);
  if (held == 0) {
    s_holdLatched = false;
  } else if (held >= kMenuHoldMs && !s_holdLatched) {
    s_holdLatched = true;
    openMenu(!s_menuOpen);
  }
  if (s_menuOpen) {
    knobDelta = 0;  // the dial doesn't control lights behind the menu
    if (millis() - s_menuRefreshMs > 1000) {
      s_menuRefreshMs = millis();
      menuRefresh();
    }
  }

  if (knobDelta != 0) applyKnob(knobDelta);
  updateBattery(false);
  const uint32_t rev = lights::revision();
  if (rev != s_seenRev) {
    s_seenRev = rev;
    render();
  }
}

}  // namespace ui
