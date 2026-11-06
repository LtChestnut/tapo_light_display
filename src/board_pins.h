// JC3636K518 pin map (from vendor pincfg.h)
#pragma once

// Display — ST77916, QSPI
#define PIN_LCD_BL   47
#define PIN_LCD_RST  21
#define PIN_LCD_CS   14
#define PIN_LCD_SCK  13
#define PIN_LCD_D0   15
#define PIN_LCD_D1   16
#define PIN_LCD_D2   17
#define PIN_LCD_D3   18

// Touch — CST816S, I2C
#define PIN_TP_SCL   12
#define PIN_TP_SDA   11
#define PIN_TP_INT   9
#define PIN_TP_RST   10

// Rotary encoder
#define PIN_ENC_A    8
#define PIN_ENC_B    7

// GPIO0 is the BOOT strap (and the I2S switch select on V2.0 boards). There is no user
// button wired to the S3, so don't use it as an input.

// Haptic DRV2605 (I2C 0x5A) shares the touch bus (SDA 11 / SCL 12).
// On V1.0 boards its EN pin is GPIO38 and must be driven high; on V2.0 EN is tied to 3V3.

// Supply sense: 10k/10k divider from the "5V" rail, which is VBAT when USB is unplugged.
#define PIN_BATT_ADC 1

// Not used yet (from the vendor schematics; see README for the full table):
//   TF card SDMMC: CLK 4, CMD 3, D0 5, D1 6, D2 42, D3 2
//   PDM mic: CLK 45, DATA 46
// The vendor demo pincfg.h that lists the SD card on 13-18 is wrong: those are the LCD.

#define LCD_WIDTH    360
#define LCD_HEIGHT   360
