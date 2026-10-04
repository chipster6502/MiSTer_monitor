// MiSTer Monitor
// Copyright (C) 2025-2026 chipster6502
// SPDX-License-Identifier: GPL-3.0-or-later

// =============================================================================
// JpegImage.h  —  JPEG screenshots and title screens, scaled like the PNG ones
// =============================================================================
// decodeJpegScaled() is decodePngScaled() for JPEG files: same size rule, same
// fill / integer modes, same sinks (see PngImage.h). ScreenScraper stores some
// screenshots as JPEG (whole systems, such as GBA); without this they would be
// drawn by the box-art path and ignore screenshot_scaling.
//
// JPEGDEC hands over blocks of 8 or 16 rows, at most 128 pixels wide; whole
// strips are assembled before their rows go to the scaler. Images far larger
// than the box are decoded at 1/2, 1/4 or 1/8 first, never below the size the
// scaler needs. Progressive JPEGs are refused: JPEGDEC only decodes their first
// scan, at 1/8 size, which would show as a blurred screenshot.
// =============================================================================

#pragma once

#include <FS.h>
#include <JPEGDEC.h>
#include "PngImage.h"

// jpg is the decoder to use: the sketch's own, which is idle between draws,
// rather than another ~18 KB of RAM.
bool decodeJpegScaled(JPEGDEC& jpg, fs::FS& fs, const char* jpgPath, int boxW, int boxH,
                      bool fill, PngSizeSink sizeSink, PngRowSink rowSink, void* user,
                      PngConvInfo* info = nullptr);
