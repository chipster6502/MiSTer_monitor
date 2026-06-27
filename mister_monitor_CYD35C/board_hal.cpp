// ============================================================================
//  board_hal.cpp  —  ESP32-3248S035C ("CYD" 3.5" capacitive) HAL definitions
//
//  board_hal.h declares the two global facade objects as `extern` so that any
//  translation unit can use them while exactly ONE copy is linked into the
//  final binary. Those single definitions live here.
//
//  Keep this the only place where `display` and `Board` are defined.
// ============================================================================

#include "board_hal.h"

// The LovyanGFX device: ST7796 panel (480x320 landscape) + GT911 cap. touch.
LGFX_CYD35 display;

// Hardware facade used throughout the .ino (display, touch, speaker stub).
BoardClass Board;
