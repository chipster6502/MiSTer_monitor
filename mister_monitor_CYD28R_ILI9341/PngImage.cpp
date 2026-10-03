// MiSTer Monitor
// Copyright (C) 2025-2026 chipster6502
// SPDX-License-Identifier: GPL-3.0-or-later

// PNG to raw RGB565 conversion. See PngImage.h for the format and size rule.

#include <Arduino.h>
#include <new>
#include <PNGdec.h>
#include "esp_heap_caps.h"
#include "PngImage.h"

// --- decoder glue -------------------------------------------------------------
// PNGdec takes plain function pointers, so the open file lives here. One
// conversion at a time, from the main loop.
static fs::FS* g_pngFs = nullptr;
static File    g_pngIn;

static void* pngConvOpen(const char* name, int32_t* size) {
  g_pngIn = g_pngFs->open(name, FILE_READ);
  if (!g_pngIn) return nullptr;
  *size = (int32_t)g_pngIn.size();
  return &g_pngIn;
}
static void pngConvClose(void*) { if (g_pngIn) g_pngIn.close(); }
static int32_t pngConvRead(PNGFILE*, uint8_t* buf, int32_t len) {
  return g_pngIn ? (int32_t)g_pngIn.read(buf, len) : 0;
}
static int32_t pngConvSeek(PNGFILE*, int32_t pos) {
  return (g_pngIn && g_pngIn.seek(pos)) ? pos : -1;
}

// Output is written in blocks of this size: row-sized writes that do not
// line up with the card's sectors are several times slower.
static const size_t PNGC_WRITE_BLOCK = 4096;

struct PngConvCtx {
  PNG* png;
  File out;
  uint8_t* wbuf;        // output block being filled
  size_t wlen;          // bytes in it
  uint32_t writeUs;     // time spent in File::write
  int srcW, srcH, outW, outH;
  bool direct;          // stored at native size: rows go straight to the file
  uint16_t* line;       // one source row as RGB565
  uint16_t* colOf;      // source column -> output column
  uint16_t* colCnt;     // source columns per output column
  uint32_t* acc;        // per output column: R5, G6, B5 sums
  uint16_t* outRow;     // one output row, high byte first
  int accRow;           // output row being accumulated, -1 when none
  int accRows;          // source rows accumulated into it
  int rowsOut;
  size_t written;       // bytes after the header
  bool ok;
};

// Output index whose span holds the centre of source index i. Monotonic, and
// every output index receives at least one source index when out <= src.
static inline int pngConvMap(int i, int src, int out) {
  return (int)(((int64_t)(2 * i + 1) * out) / (2 * src));
}

static bool pngConvFlushBlock(PngConvCtx* c) {
  if (!c->wlen) return true;
  uint32_t t0 = micros();
  bool ok = (c->out.write(c->wbuf, c->wlen) == c->wlen);
  c->writeUs += micros() - t0;
  c->written += c->wlen;
  c->wlen = 0;
  return ok;
}

static bool pngConvPut(PngConvCtx* c, const uint8_t* data, size_t len) {
  while (len) {
    size_t n = PNGC_WRITE_BLOCK - c->wlen;
    if (n > len) n = len;
    memcpy(c->wbuf + c->wlen, data, n);
    c->wlen += n; data += n; len -= n;
    if (c->wlen == PNGC_WRITE_BLOCK && !pngConvFlushBlock(c)) return false;
  }
  return true;
}

static bool pngConvFlushRow(PngConvCtx* c) {
  for (int ox = 0; ox < c->outW; ox++) {
    uint32_t n = (uint32_t)c->colCnt[ox] * (uint32_t)c->accRows;
    uint32_t* a = &c->acc[ox * 3];
    uint16_t r = (uint16_t)((a[0] + n / 2) / n);
    uint16_t g = (uint16_t)((a[1] + n / 2) / n);
    uint16_t b = (uint16_t)((a[2] + n / 2) / n);
    uint16_t v = (uint16_t)((r << 11) | (g << 5) | b);
    c->outRow[ox] = (uint16_t)((v >> 8) | (v << 8));
    a[0] = a[1] = a[2] = 0;
  }
  if (!pngConvPut(c, (const uint8_t*)c->outRow, (size_t)c->outW * 2)) return false;
  c->rowsOut++;
  return true;
}

static int pngConvDraw(PNGDRAW* d) {
  PngConvCtx* c = (PngConvCtx*)d->pUser;
  if (!c->ok) return 0;

  if (c->direct) {
    // Alpha, if any, is blended onto black: the slot background.
    c->png->getLineAsRGB565(d, c->line, PNG_RGB565_BIG_ENDIAN, 0x000000);
    if (!pngConvPut(c, (const uint8_t*)c->line, (size_t)c->srcW * 2)) { c->ok = false; return 0; }
    c->rowsOut++;
    return 1;
  }

  int oy = pngConvMap(d->y, c->srcH, c->outH);
  if (oy != c->accRow) {
    if (c->accRow >= 0 && !pngConvFlushRow(c)) { c->ok = false; return 0; }
    c->accRow = oy;
    c->accRows = 0;
  }
  c->png->getLineAsRGB565(d, c->line, PNG_RGB565_LITTLE_ENDIAN, 0x000000);
  for (int x = 0; x < c->srcW; x++) {
    uint16_t p = c->line[x];
    uint32_t* a = &c->acc[c->colOf[x] * 3];
    a[0] += p >> 11;
    a[1] += (p >> 5) & 0x3F;
    a[2] += p & 0x1F;
  }
  c->accRows++;
  return 1;
}

// Stored size for a source of w x h in a slot of slotW x slotH (see the rule
// at the top of this file).
static void pngPlanSize(int w, int h, int slotW, int slotH, int& outW, int& outH) {
  if (w <= slotW && h <= slotH) { outW = w; outH = h; return; }
  if ((int64_t)w * slotH > (int64_t)h * slotW) {
    outW = slotW;
    outH = (int)(((int64_t)h * slotW + w / 2) / w);
  } else {
    outH = slotH;
    outW = (int)(((int64_t)w * slotH + h / 2) / h);
  }
  if (outW < 1) outW = 1;
  if (outH < 1) outH = 1;
}

bool convertPngTo565(fs::FS& fs, const char* pngPath, const char* rawPath,
                     int slotW, int slotH, PngConvInfo* info) {
  // Not 'local': zutil.h, pulled in by PNGdec.h, defines it as a macro.
  PngConvInfo scratch;
  PngConvInfo& in = info ? *info : scratch;
  in = PngConvInfo();
  uint32_t t0 = millis();
  in.largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);

  if (!fs.exists(pngPath)) { in.error = "no such file"; return false; }

  // The header gives the size, and the size decides every other buffer. Its
  // first 24 bytes are the signature and IHDR: width and height at 16 and 20.
  int w = 0, h = 0;
  {
    File f = fs.open(pngPath, FILE_READ);
    uint8_t hd[24];
    if (f && f.read(hd, 24) == 24 && hd[0] == 0x89 && hd[1] == 'P' &&
        hd[12] == 'I' && hd[13] == 'H' && hd[14] == 'D' && hd[15] == 'R') {
      w = (int)((uint32_t)hd[16] << 24 | (uint32_t)hd[17] << 16 | hd[18] << 8 | hd[19]);
      h = (int)((uint32_t)hd[20] << 24 | (uint32_t)hd[21] << 16 | hd[22] << 8 | hd[23]);
    }
    if (f) f.close();
  }
  if (w <= 0 || h <= 0 || w > 8192 || h > 8192) { in.error = "unsupported or damaged PNG"; return false; }

  PngConvCtx c = {};
  c.srcW = in.srcW = w;
  c.srcH = in.srcH = h;
  c.accRow = -1;
  pngPlanSize(w, h, slotW, slotH, c.outW, c.outH);
  in.outW = c.outW;
  in.outH = c.outH;
  c.direct = (c.outW == c.srcW && c.outH == c.srcH);

  // Everything except the decoder first, output file included.
  String tmpPath = String(rawPath) + ".tmp";
  bool ok = false;
  c.wbuf = (uint8_t*)malloc(PNGC_WRITE_BLOCK);
  c.line = (uint16_t*)malloc((size_t)c.srcW * 2);
  if (!c.direct) {
    c.colOf  = (uint16_t*)malloc((size_t)c.srcW * 2);
    c.colCnt = (uint16_t*)calloc(c.outW, 2);
    c.acc    = (uint32_t*)calloc((size_t)c.outW * 3, 4);
    c.outRow = (uint16_t*)malloc((size_t)c.outW * 2);
  }
  if (!c.wbuf || !c.line || (!c.direct && (!c.colOf || !c.colCnt || !c.acc || !c.outRow))) {
    in.error = "row buffer allocation failed";
  } else {
    if (!c.direct) {
      for (int x = 0; x < c.srcW; x++) {
        c.colOf[x] = (uint16_t)pngConvMap(x, c.srcW, c.outW);
        c.colCnt[c.colOf[x]]++;
      }
    }
    if (fs.exists(tmpPath)) fs.remove(tmpPath);
    c.out = fs.open(tmpPath, FILE_WRITE);
    uint8_t head[4] = { (uint8_t)(c.outW & 0xFF), (uint8_t)(c.outW >> 8),
                        (uint8_t)(c.outH & 0xFF), (uint8_t)(c.outH >> 8) };
    if (!c.out || c.out.write(head, 4) != 4) {
      in.error = "cannot write output";
    } else if (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < sizeof(PNG) + 4096) {
      in.error = "not enough contiguous heap";
    } else if (!(c.png = new (std::nothrow) PNG())) {
      in.error = "PNG allocation failed";
    } else {
      g_pngFs = &fs;
      if (c.png->open(pngPath, pngConvOpen, pngConvClose, pngConvRead, pngConvSeek,
                      pngConvDraw) != PNG_SUCCESS ||
          c.png->getWidth() != w || c.png->getHeight() != h) {
        in.error = "unsupported or damaged PNG";
      } else {
        in.pixelType = c.png->getPixelType();
        in.bpp = c.png->getBpp();
        c.ok = true;
        int rc = c.png->decode(&c, 0);
        if (c.ok && !c.direct && c.accRow >= 0 && !pngConvFlushRow(&c)) c.ok = false;
        if (c.ok && !pngConvFlushBlock(&c)) c.ok = false;
        if (rc != PNG_SUCCESS) in.error = "decode failed";
        else if (!c.ok) in.error = "write failed";
        else if (c.rowsOut != c.outH ||
                 c.written != (size_t)c.outW * c.outH * 2) in.error = "incomplete output";
        else ok = true;
      }
      c.png->close();
      // Freed first, while nothing else has been released yet.
      delete c.png;
      c.png = nullptr;
    }
    if (c.out) c.out.close();
  }

  if (ok) {
    if (fs.exists(rawPath)) fs.remove(rawPath);
    ok = fs.rename(tmpPath.c_str(), rawPath);
    if (!ok) in.error = "rename failed";
  }
  if (!ok && fs.exists(tmpPath)) fs.remove(tmpPath);

  free(c.outRow); free(c.acc); free(c.colCnt); free(c.colOf); free(c.line); free(c.wbuf);
  in.writeMs = c.writeUs / 1000;
  in.ms = millis() - t0;
  return ok;
}
