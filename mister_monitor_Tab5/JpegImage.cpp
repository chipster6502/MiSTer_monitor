// MiSTer Monitor
// Copyright (C) 2025-2026 chipster6502
// SPDX-License-Identifier: GPL-3.0-or-later

// JPEG decoding scaled to a box. See JpegImage.h.

#include <Arduino.h>
#include <JPEGDEC.h>
#include "esp_heap_caps.h"
#include "JpegImage.h"

static const uint32_t JPGC_CAPS = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;

// JPEGDEC takes plain function pointers, so the open file lives here. One
// decode at a time, from the main loop.
static fs::FS* g_jpgFs = nullptr;
static File    g_jpgIn;

static void* jpgOpen(const char* name, int32_t* size) {
  g_jpgIn = g_jpgFs->open(name, FILE_READ);
  if (!g_jpgIn) return nullptr;
  *size = (int32_t)g_jpgIn.size();
  return &g_jpgIn;
}
static void jpgClose(void*) { if (g_jpgIn) g_jpgIn.close(); }
static int32_t jpgRead(JPEGFILE* f, uint8_t* buf, int32_t len) {
  int32_t n = g_jpgIn ? (int32_t)g_jpgIn.read(buf, len) : 0;
  if (n > 0) f->iPos += n;
  return n;
}
static int32_t jpgSeek(JPEGFILE* f, int32_t pos) {
  if (!g_jpgIn || !g_jpgIn.seek(pos)) return -1;
  f->iPos = pos;
  return pos;
}

struct JpgCtx {
  ImgScaler* sc;
  uint16_t* strip;      // one MCU row of the decoded image, little-endian
  int w, h;             // decoded size (after any 1/2, 1/4, 1/8)
  bool ok;
};

// Blocks arrive left to right along each MCU row; the one that reaches the
// right edge completes the strip, whose rows then go to the scaler.
static int jpgDraw(JPEGDRAW* d) {
  JpgCtx* c = (JpgCtx*)d->pUser;
  if (!c->ok) return 0;
  int used = d->iWidthUsed;
  if (d->x < 0 || d->x + used > c->w || d->iHeight > 16 || d->iHeight < 1) {
    c->ok = false;
    return 0;
  }
  for (int r = 0; r < d->iHeight; r++)
    memcpy(c->strip + r * c->w + d->x, d->pPixels + r * d->iWidth, (size_t)used * 2);
  if (d->x + used == c->w) {
    for (int r = 0; r < d->iHeight; r++) {
      if (!imgScalerRow(c->sc, c->strip + r * c->w, false)) { c->ok = false; return 0; }
    }
  }
  return 1;
}

bool decodeJpegScaled(JPEGDEC& jpgDec, fs::FS& fs, const char* jpgPath, int boxW, int boxH,
                      bool fill, PngSizeSink sizeSink, PngRowSink rowSink, void* user,
                      PngConvInfo* info) {
  PngConvInfo scratch;
  PngConvInfo& in = info ? *info : scratch;
  in = PngConvInfo();
  uint32_t t0 = millis();
  in.largestBlock = heap_caps_get_largest_free_block(JPGC_CAPS);
  if (!fs.exists(jpgPath)) { in.error = "no such file"; return false; }

  JPEGDEC* jpg = &jpgDec;
  JpgCtx c = {};
  bool ok = false;
  g_jpgFs = &fs;

  if (!jpg->open(jpgPath, jpgOpen, jpgClose, jpgRead, jpgSeek, jpgDraw)) {
    in.error = "unreadable JPEG";
  } else if (jpg->getJPEGType() == JPEG_MODE_PROGRESSIVE) {
    in.error = "progressive JPEG";
    jpg->close();
  } else {
    int w = jpg->getWidth(), h = jpg->getHeight();
    in.srcW = w;
    in.srcH = h;
    in.bpp = 8;
    // Largest 1/2^s decode that is still at least the size the scaler needs.
    int needW = 0, needH = 0, shift = 0;
    pngFitSize(w, h, boxW, boxH, fill, &needW, &needH);
    while (shift < 3) {
      int n = shift + 1, adj = (1 << n) - 1;
      if (((w + adj) >> n) < needW || ((h + adj) >> n) < needH) break;
      shift = n;
    }
    int adj = (1 << shift) - 1;
    c.w = (w + adj) >> shift;
    c.h = (h + adj) >> shift;
    static const int scaleOpt[4] = { 0, JPEG_SCALE_HALF, JPEG_SCALE_QUARTER, JPEG_SCALE_EIGHTH };

    c.sc = imgScalerBegin(c.w, c.h, boxW, boxH, fill, sizeSink, rowSink, user,
                          &in.outW, &in.outH, &in.error);
    if (c.sc) {
      c.strip = (uint16_t*)heap_caps_malloc((size_t)c.w * 16 * 2, JPGC_CAPS);
      if (!c.strip) {
        in.error = "strip buffer allocation failed";
      } else {
        c.ok = true;
        jpg->setPixelType(RGB565_LITTLE_ENDIAN);
        jpg->setUserPointer(&c);
        int rc = jpg->decode(0, 0, scaleOpt[shift]);
        if (!c.ok) in.error = "aborted by the sink";
        else if (rc != 1) in.error = "decode failed";
        else ok = true;
      }
      heap_caps_free(c.strip);
      if (!imgScalerEnd(c.sc, &in.sinkMs) && ok) {
        ok = false;
        in.error = "incomplete output";
      }
    }
    jpg->close();
  }
  in.ms = millis() - t0;
  return ok;
}
