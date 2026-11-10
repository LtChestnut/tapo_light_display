# Tapo Light Knob

Standalone firmware that turns a **Guition JC3636K518** ESP32-S3 round display knob into a wall/desk controller for **TP-Link Tapo smart bulbs**. It talks to the bulbs directly over the local network (Tapo KLAP protocol) — no Home Assistant, hub, or cloud round-trip at runtime.

Target setup: 3 × Tapo **L530/L530E** colour bulbs (KLAP v2, local HTTP).

> **Status:** working end to end on the device: controls the real bulbs, battery powered, deep sleep with twist-to-wake. See [Roadmap](#roadmap) for what's next.

---

## Behaviour

The **dial is the main control**. The touchscreen is used for swipes only, plus one button on the "All lights" page, so a missed swipe can't switch a light.

- **Swipe left/right:** "All lights" → each bulb. Dots at the bottom show the page.
- **All lights page:** a small power button. Tapping it when anything is on remembers which bulbs were on, then switches everything off. Tapping again switches back on only that remembered set (stored in flash). Turning the dial right or left does the same.
- **Bulb page:** swipe up/down to cycle **Brightness → Temperature → Colour**. Dots on the right show the mode.
  - **Brightness:** arc, 0–100 %. Turning down to 0 % switches the bulb off; turning up from 0 % switches it on.
  - **Temperature:** warm-to-cool gradient ring with an arrow (2500–6500 K, from the bulb).
  - **Colour:** hue wheel with an arrow, full saturation.
  - The dial accelerates: 2 % / 100 K / 5° per click when turning slowly, ×2 when brisk, ×4 when spinning.
- **Haptics:** short pulses on page/mode change, on/off, and at the end of a range.
- **Battery:** percentage in the gap at the bottom of the dial; a USB symbol when plugged in.
- Changes made in the Tapo app appear on the knob within about 5 s.

**Secret menu:** hold a finger still on the centre of the screen for 10 s. It shows the version and git hash, Wi-Fi network/IP, each light with its IP and whether it's reachable, and the battery voltage. It also holds the **battery calibration**: charge fully, press Start, unplug, and leave it until it dies (a few hours at full load). On the next boot the knob turns the logged discharge into its own voltage → % curve. `cal status` / `cal dump` on the serial port show the result and the raw log.

**Low battery:** below 3.3 V (under load) the knob shows "Battery low – please charge" and goes to sleep, to avoid deep-discharging the cell. Twisting it still wakes it, but it only shows the message again until it has been charged. The battery % reads 0 % at the cutoff. On the measured cell this costs about 7 % of the runtime: at full load it's ~3 h 15 min of 3 h 31 min, about 200 mA.

**Power:**

| State | When | Notes |
|---|---|---|
| Active | any input | full speed |
| Screen off | 20 s idle | Wi-Fi power-save; the next input responds instantly. The waking input does nothing else. |
| Deep sleep | 45 s idle, **on battery only** | Twist the dial to wake. Wi-Fi is back in ~0.5 s and the screen in ~1 s, on the same page. |

On USB it never deep sleeps, so the serial port stays up for flashing. Timeouts and touch-to-wake are set in `src/config.h`.

## Hardware

**Device:** Guition JC3636K518 (AliExpress listing "JC3636K518C-I-YR1"). A near-identical Waveshare variant exists ("ESP32-S3 Knob Touch LCD 1.8").

| Item | Detail |
|---|---|
| Main MCU | ESP32-S3R8 — dual-core 240 MHz, 8 MB **octal** PSRAM, 16 MB QIO flash (W25Q128) |
| Secondary MCU | ESP32-U4WDH (classic Bluetooth audio in stock firmware). **Not used by this project.** |
| Display | 1.8" round 360×360 IPS, **ST77916** over QSPI, needs a vendor init sequence + colour inversion |
| Touch | **CST816S** capacitive, I2C |
| Knob | Quadrature rotary encoder ring (no push switch on the S3 side) |
| Haptics | **DRV2605** LRA driver on the touch I2C bus |
| Other | PDM microphone, PCM5100A I2S DAC (shared with the ESP32 via a CH445P switch), TF card, battery ADC |
| Power | 5 V USB-C, ~1.3 W |

### ESP32-S3 pin map

Verified against the vendor schematics (`Resources/6-Schematic diagram/`, V1.0 and V2.0) and demo `pincfg.h`. The core peripherals are identical on both board revisions.

| Function | GPIO |
|---|---|
| LCD QSPI SCK / CS | 13 / 14 |
| LCD QSPI D0–D3 | 15, 16, 17, 18 |
| LCD RST | 21 |
| LCD backlight (PWM, via MOSFET) | 47 |
| Touch I2C SDA / SCL | 11 / 12 (5.1 kΩ pull-ups on board) |
| Touch INT / RST | 9 / 10 |
| Encoder A / B | 8 / 7 (10 kΩ pull-ups on board) |
| Haptic DRV2605 | shares touch I2C (11/12); TRIG tied to GND |
| TF card (SDMMC 4-bit) CLK / CMD | 4 / 3 |
| TF card D0 / D1 / D2 / D3 | 5 / 6 / 42 / 2 |
| PDM mic CLK / DATA | 45 / 46 |
| Battery ADC (5 V ÷ 2 divider) | 1 |
| USB D− / D+ (native) | 19 / 20 |
| GPIO0 | Boot strap (10 kΩ pull-up). Also the I2S switch select on V2.0 |

Revision-specific pins (only matter if we use these peripherals):

| Function | V1.0 | V2.0 |
|---|---|---|
| UART link to secondary ESP32 (S3 TX / RX) | 40 / 39 | 38 / 48 |
| DRV2605 EN | GPIO38 (must be driven high) | tied to 3V3 |
| LCD TE | 48 | not connected |
| I2S DAC BCK / WS / DIN | — | 39 / 40 / 41 |

> The `pincfg.h` in `ST77916_LVGL_DEMO` lists the SD card on 13–18. That is **wrong** for both revisions: those pins are the LCD. The `ST77916_LVGL_IMG_DEMO` copy has the correct SD pins.

## Connecting & flashing

The USB-C port is wired to **both** chips; which one you reach depends on **which way round the plug is inserted**:

| Windows shows | Chip | Use it? |
|---|---|---|
| **USB Serial Device (COMx)** — generic | ESP32-S3 (native USB-Serial/JTAG) | **Yes — this is the one we flash** |
| USB-SERIAL CH340 (COMx) | Secondary ESP32 via CH340 | No |

If you see the CH340, flip the USB-C plug. Native USB needs no driver.

Vendor notes, translated (`Resources/9-Burn/Burn operation instructions/烧录说明.txt`):
- The base has a **power switch**. It must be **ON (slid left)** while flashing.
- Power-cycle after flashing. The project's upload script resets the chip for you, so this is normally unnecessary.

```
pio run -t upload --upload-port COM7     # build + flash + reset
pio device monitor                       # serial log at 115200
pio test -e jc3636k518 --upload-port COM7 --test-port COM7   # on-device KLAP tests
```

If PlatformIO isn't on your PATH, use `%USERPROFILE%\.platformio\penv\Scripts\pio.exe`. After `pio test`, re-flash the app.

Why there's a custom reset: esptool's default post-flash reset makes the S3's native USB port drop off mid-reset. PlatformIO then reports a perfectly good flash as FAILED. `scripts/reset_after_upload.py` resets the chip cleanly instead.

If an upload fails to connect, put the S3 into download mode manually: hold BOOT (GPIO0) while resetting or plugging in. The stock vendor firmware is in `Resources/9-Burn/.../ESP32_S3烧录/ESP32_S3_1.85KNOB.bin` if you ever need to restore it. Back up your own unit's flash with `esptool read_flash` before the first overwrite; this is optional, since the vendor bin exists.

## Toolchain

- **VS Code + PlatformIO IDE extension**: installs the compiler, esptool and a serial monitor.
- **Platform:** [pioarduino](https://github.com/pioarduino/platform-espressif32), the community fork. The official `espressif32` platform is stuck on Arduino-ESP32 2.x, and the vendor display code needs **Arduino-ESP32 3.x** (the vendor tested 3.1.0).
- **Libraries**, installed automatically from `platformio.ini`:
  - `lvgl` **8.4.0**. Do not use 9.x; the vendor demo and its `lv_conf.h` are written for v8.
  - `ESP32_Display_Panel` v1.0.1, `ESP32_IO_Expander` v1.0.1 and `esp-lib-utils` v0.1.2, pulled from GitHub tags. The PlatformIO registry copies are years out of date, and the vendor's bundled IO_Expander 1.1.0 conflicts with Display_Panel 1.0.1's dependencies.
  - `ArduinoJson` 7
- **Board settings**, from the vendor's Arduino screenshot: ESP32-S3 Dev Module, 240 MHz, flash QIO 120 MHz, 16 MB, **OPI PSRAM**, USB CDC on boot enabled.

## Tapo setup

1. Add the bulbs in the Tapo app as normal and give each one a **DHCP reservation** on your router, so their IPs don't change.
2. In the Tapo app, enable **Me → Third-Party Services → Third-Party Compatibility**. Newer bulb firmware refuses local control without it.
3. Fill in `src/secrets.h`, which is git-ignored and already created with your three bulb IPs:
   - Wi-Fi SSID and password. It must be **2.4 GHz**; the ESP32-S3 has no 5 GHz radio.
   - Your **Tapo account email and password**. They're only used on the device to derive the local KLAP auth hash; nothing goes to the cloud.

   Until real values are in, the screen shows "Edit secrets.h" and the network stays off.
4. To find bulb IPs again, run `kasa discover list` from [python-kasa](https://github.com/python-kasa/python-kasa). It lists every Tapo device, model and encryption type without needing credentials.

## Repository layout

```
platformio.ini        Build config (pioarduino, board/flash/PSRAM settings, libs)
src/
  main.cpp            setup()/loop()
  hal.*               Display, touch, backlight, knob -> LVGL
  board_pins.h        All GPIO assignments
  st77916_init.h      Vendor LCD init sequence
  tapo_klap.*         Tapo KLAP v2 client (crypto + HTTP)
  lights.*            Wi-Fi + background bulb service (queue, rate limit, polling)
  ui.*                LVGL light-control screen
  secrets.example.h   Template for src/secrets.h (git-ignored)
  config.h            Sleep timeouts, deep sleep on USB, touch-to-wake
include/lv_conf.h     LVGL 8 config
test/test_klap/       On-device Unity tests (vectors from python-kasa)
scripts/              PlatformIO helper scripts
tools/secondary_sleep/ ESP-IDF image that puts the board's second ESP32 into permanent deep sleep
Resources/            (git-ignored, ~1 GB) vendor package JC3636K518CN_knob_EN: datasheets,
                      schematics, demos, stock firmware. Download it separately for reference.
CLAUDE.md             Working notes for Claude Code
```

## Roadmap

Done: bring-up, Wi-Fi, KLAP client, controller UI, colour/temperature modes, haptics, battery indicator, display sleep, deep sleep with twist-to-wake, second ESP32 put to sleep.

Next:
1. **Wake check:** fixed IP + Wi-Fi-first boot cut a cold boot to Wi-Fi from 6.7 s to 0.5 s. Confirm the same on a real deep-sleep wake, and look at the occasional "no HTTP status" on the first request after a wake.
2. **Run the battery calibration** (secret menu), then measure real battery life (700 mAh cell) and decide whether touch-to-wake is affordable.
3. On-device setup (captive portal) instead of `secrets.h`; OTA updates.

## References

- [nkinnan — Waveshare / Guition K5 knob notes](https://github.com/nkinnan/Waveshare-ESP32-S3-Knob-Touch-LCD-1.8_and_Guition-K5-Knob-Series-JC3636K518). Hardware notes and links. The S3 flash *read* is unreliable above 4 KB chunks; writes are unaffected as far as known.
- [python-kasa](https://github.com/python-kasa/python-kasa): reference implementation of the Tapo KLAP protocol (`kasa/transports/klaptransport.py`).
- [tapo (Rust)](https://github.com/mihai-dinculescu/tapo): second reference implementation, including a request/response schema for bulbs.
- [Espressif ESP32_Display_Panel](https://github.com/esp-arduino-libs/ESP32_Display_Panel)
