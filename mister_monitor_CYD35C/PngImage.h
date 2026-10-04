// MiSTer Monitor
// Copyright (C) 2025-2026 chipster6502
// SPDX-License-Identifier: GPL-3.0-or-later

// =============================================================================
// PngImage.h  —  PNG decoding, scaled to the game image box, as RGB565 rows
// =============================================================================
// decodePngScaled() hands the decoded image to a caller-supplied sink one row
// at a time, top to bottom, as RGB565 pixels with the high byte first (the
// order the panel takes). convertPngTo565() is the same with a file as the
// sink: a 4-byte header (width, height, little-endian) followed by the rows,
// the raw format drawStandbyLogoRaw() reads.
//
// Size rule for a box of boxW x boxH (fill chooses between the two modes of
// the screenshot_scaling setting):
//   * larger than the box -> area average down to the largest size that fits,
//                            keeping the aspect ratio. An exact 1/2 comes out
//                            as a 2x2 average. Both modes.
//   * fits, fill          -> grown to the largest size that fits with a sharp
//                            bilinear scale: every pixel repeated k times (k the
//                            next whole factor up), then area averaged down.
//                            Pixels stay crisp; only some edges soften by one.
//   * fits, integer       -> rows at native size; integer upscaling, if any, is
//                            left to the sink, so output never exceeds the source.
// Never nearest neighbour at a fractional factor: it would draw some rows and
// columns twice as thick as others.
//
// The decoder needs one contiguous block of sizeof(PNG) bytes (~48 KB: zlib
// window, inflate state, palette, two row buffers). It is allocated per call,
// after everything else, and freed first, so nothing allocated meanwhile can
// end up splitting the block it leaves behind. A call that cannot get it fails
// cleanly. All buffers come from internal RAM; see PngImage.cpp for the PSRAM
// fallback a board can opt into. PNG_MAX_BUFFERED_PIXELS comes from build_opt.h so that the library
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
  int outW = 0, outH = 0;     // output size
  int pixelType = -1;         // PNG colour type (3 = palette, 2 = RGB, ...)
  int bpp = 0;                // bits per sample
  uint32_t ms = 0;            // whole call
  uint32_t sinkMs = 0;        // part of it spent in the sink
  uint32_t largestBlock = 0;  // largest free internal heap block on entry
  const char* error = "";     // empty on success
};

// Reads the size and PNG colour type (0 gray, 2 RGB, 3 palette, 4 gray+alpha,
// 6 RGBA) from the header without decoding. False if the file is missing or
// is not a PNG.
bool pngReadHeader(fs::FS& fs, const char* pngPath, int* w, int* h, int* colorType);

// Size decodePngScaled() produces for a w x h source in a boxW x boxH box.
void pngFitSize(int w, int h, int boxW, int boxH, bool fill, int* outW, int* outH);

// False for a PNG this decoder cannot read: interlaced, or more than 8 bits
// per sample. Checks the header only.
bool pngIsSupported(fs::FS& fs, const char* pngPath);

// Receives output row y (outW pixels, RGB565 high byte first). The buffer is
// only valid during the call. Return false to abort the decode.
typedef bool (*PngRowSink)(void* user, int y, const uint16_t* row, int w);

// Called once the output size is known and before any row, so the sink can
// position itself or refuse (return false). May be null.
typedef bool (*PngSizeSink)(void* user, int outW, int outH);

// Decodes pngPath scaled for a boxW x boxH box. Returns false, with
// info->error set, on any failure, including a sink that refuses or aborts.
bool decodePngScaled(fs::FS& fs, const char* pngPath, int boxW, int boxH, bool fill,
                     PngSizeSink sizeSink, PngRowSink rowSink, void* user,
                     PngConvInfo* info = nullptr);

// Row-by-row scaler behind decodePngScaled(), for other decoders (JpegImage).
// Feed it every source row, top to bottom, as RGB565 (bigEndian says which
// byte order), then call imgScalerEnd(), which also frees it. Rows reach the
// sink as decodePngScaled() delivers them. imgScalerDirect() is true when the
// output is the source unchanged, so a decoder can hand over rows already in
// the sink's big-endian order.
struct ImgScaler;
ImgScaler* imgScalerBegin(int srcW, int srcH, int boxW, int boxH, bool fill,
                          PngSizeSink sizeSink, PngRowSink rowSink, void* user,
                          int* outW, int* outH, const char** error);
bool imgScalerDirect(const ImgScaler* s);
bool imgScalerRow(ImgScaler* s, const uint16_t* row, bool bigEndian);
// True when every row went through; adds the time spent in the sink to *sinkMs.
bool imgScalerEnd(ImgScaler* s, uint32_t* sinkMs);

// Converts pngPath into rawPath (written as rawPath + ".tmp", then renamed).
// Returns false, with info->error set, on any failure; rawPath is then left
// untouched.
bool convertPngTo565(fs::FS& fs, const char* pngPath, const char* rawPath,
                     int boxW, int boxH, bool fill, PngConvInfo* info = nullptr);
