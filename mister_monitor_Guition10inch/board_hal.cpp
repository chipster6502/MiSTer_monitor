// MiSTer Monitor
// Copyright (C) 2025-2026 chipster6502
// SPDX-License-Identifier: GPL-3.0-or-later

// ============================================================================
//  board_hal.cpp  -  Guition JC8012P4A1C HAL definitions
//
//  board_hal.h declares the two global facade objects as `extern` so that any
//  translation unit can use them while exactly ONE copy is linked into the
//  final binary. Those single definitions live here.
//
//  Keep this the only place where `display` and `Board` are defined.
// ============================================================================

#include "board_hal.h"

// The LovyanGFX device: JD9365 DSI panel, 1280x720 landscape window.
m5gfx_user::LGFX_JC8012P4A1C display;

// Hardware facade used throughout the .ino (display, touch, speaker stub).
BoardClass Board;
