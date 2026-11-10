// Battery voltage, state-of-charge estimate, and the discharge calibration that produces a
// voltage -> % curve for this particular cell.
//
// Calibration: start it from the secret menu (hold the screen centre for 10 s), unplug, and
// leave it. The knob runs flat out (screen at full brightness, Wi-Fi active, no sleep) and
// appends (time, voltage) to /cal.bin in flash every 30 s until the battery dies. On the
// next boot the log is turned into an 11-point curve (0..100 % in 10 % steps, 0 % = where
// it died) stored in NVS and used from then on. `cal dump` on the serial port prints the
// raw log.
#pragma once
#include <stdint.h>

namespace battery {

void begin();    // mount the log FS, load the curve, finish a calibration that ran out
void service();  // call from loop(): sampling, calibration logging, serial commands

float volts();   // smoothed supply voltage
bool onUsb();    // supply is USB (we then can't see the battery itself)
int percent();   // 0-100 estimate; meaningless while onUsb()
bool calibrated();  // using a measured curve rather than the generic Li-ion one

// Low-battery cutoff (config.h). bootIsEmpty() needs no other init and is meant to be called
// first in setup(): true = the battery was already cut off and hasn't recovered, so show
// the message and sleep again without starting anything else. empty() becomes true while
// running once the loaded voltage stays below the cutoff (or after `batt low` on serial).
bool bootIsEmpty();
bool empty();
void clearEmpty();  // e.g. USB was plugged in: back to normal

enum class CalState : uint8_t { Idle, Armed, Running };
CalState calState();
void calStart();  // arm: logging begins when USB is unplugged
void calStop();   // abort (log kept, no curve generated)
uint32_t calElapsedS();
uint32_t calSamples();
const char *calLastResult();  // human-readable outcome of the last run ("" if none)

}  // namespace battery
