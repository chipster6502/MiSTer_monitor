// Single-translation-unit definitions for the globals declared in board_hal.h.
// Lives in its own .cpp so multiple includes never produce duplicate symbols.
#include "board_hal.h"

LGFX_CYD display;
BoardClass Board;
