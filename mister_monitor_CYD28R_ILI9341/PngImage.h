// MiSTer Monitor
// Copyright (C) 2025-2026 chipster6502
// SPDX-License-Identifier: GPL-3.0-or-later

// =============================================================================
// PngImage.h  —  PNG to raw RGB565 (.565) conversion for the game image slot
// =============================================================================
// Writes the raw format drawStandbyLogoRaw() reads: a 4-byte header (width,
// height, little-endian) followed by RGB565 pixels, high byte first, row by
// row.
//
// Size rule for a slot of slotW x slotH:
//   * fits at 1x  -> stored at native size; integer upscaling, if any, is
//                    done when drawing, so the file never exceeds the source;
//   * otherwise   -> area average down to the largest size that fits, keeping
//                    the aspect ratio. An exact 1/2 comes out as a 2x2 average.
// Downscaling never drops pixels (no nearest neighbour), so text and sprites
// keep their shape at the cost of slight softening.
//
// The decoder needs one contiguous block of sizeof(PNG) bytes (~48 KB: zlib
// window, inflate state, palette, two row buffers). It is allocated per
// conversion, after everything else, and freed first, so nothing allocated
// meanwhile can end up splitting the block it leaves behind. A conversion
// that cannot get it fails cleanly. PNG_MAX_BUFFERED_PIXELS comes from build_opt.h so that the library
// and the sketch agree on the object layout.
//
// Interlaced and 16-bit-per-channel PNGs are not supported by the decoder;
// they fail at open like any other unreadable file.
//
// The implementation lives in PngImage.cpp, its own translation unit: the
// decoder pulls in zlib's headers, whose macros and global names must not
// reach the sketch.
// =============================================================================

#pragma once

#include <FS.h>

struct PngConvInfo {
  int srcW = 0, srcH = 0;     // source size
  int outW = 0, outH = 0;     // stored size
  int pixelType = -1;         // PNG colour type (3 = palette, 2 = RGB, ...)
  int bpp = 0;                // bits per sample
  uint32_t ms = 0;            // whole conversion, open to rename
  uint32_t writeMs = 0;       // part of it spent writing to the card
  uint32_t largestBlock = 0;  // largest free heap block before allocating
  const char* error = "";     // empty on success
};

// Converts pngPath into rawPath (written as rawPath + ".tmp", then renamed).
// Returns false, with info->error set, on any failure; rawPath is then left
// untouched.
bool convertPngTo565(fs::FS& fs, const char* pngPath, const char* rawPath,
                     int slotW, int slotH, PngConvInfo* info = nullptr);
