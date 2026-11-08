// Touch panel: an XPT2046 on its own SPI bus. Only "was tapped" is needed,
// so there is no calibration and nothing to do for a flipped display.
#pragma once

#include <Arduino.h>

namespace touch {

void begin();
// True once per tap (finger down after a release), with debouncing. Call from
// loop(); cheap when nothing is touching.
bool tapped();

} // namespace touch
