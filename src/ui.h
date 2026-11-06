// Light-control screen. The dial is the main control; touch is swipes + the All-lights button.
//
//  Pages (swipe left/right):  All lights -> bulb 1 -> bulb 2 -> ...
//    All lights:  tap = all on/off. Knob does nothing.
//    A bulb:      swipe up/down cycles   Brightness -> Temperature -> Colour
//                 knob adjusts the current mode (brightness 0% = off)
//                 tap = toggle on/off
#pragma once
#include <stdint.h>

namespace ui {

void begin();

// Call from the LVGL thread every loop with the knob detents since the last call.
void tick(int32_t knobDelta);

// True once the UI has been idle long enough to deep sleep (see config.h).
bool wantsDeepSleep();

}  // namespace ui
