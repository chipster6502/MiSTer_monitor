// MiSTer Monitor
// Copyright (C) 2025-2026 chipster6502
// SPDX-License-Identifier: GPL-3.0-or-later

// PNG decoding scaled to a box. See PngImage.h for the size rule.

#include <Arduino.h>
#include <new>
#include <PNGdec.h>
#include "esp_heap_caps.h"
#include "PngImage.h"

// build_opt.h must reach this file and the library alike. Without it the
// decoder's row buffers only hold 320 RGBA pixels and wider RGB images are
// rejected at open.
#if PNG_MAX_BUFFERED_PIXELS < ((640 * 4 + 1) * 2)
#error "PNG_MAX_BUFFERED_PIXELS is too small: build_opt.h was not applied"
#endif

// Internal RAM, asked for explicitly: on boards with PSRAM the default
// allocator may hand out PSRAM, which is slower everywhere and not usable at
// all on some. A board whose PSRAM is usable, and whose internal RAM may lack
// a contiguous block for the decoder, opts into it as a second choice with
// -DPNGIMAGE_SPIRAM_FALLBACK in its build_opt.h.
static const uint32_t PNGC_CAPS = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;

static void* pngAlloc(size_t bytes) {
  void* p = heap_caps_malloc(bytes, PNGC_CAPS);
#ifdef PNGIMAGE_SPIRAM_FALLBACK
  if (!p) p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
  return p;
}

static void* pngCalloc(size_t n, size_t size) {
  void* p = pngAlloc(n * size);
  if (p) memset(p, 0, n * size);
  return p;
}

static bool pngHasBlock(size_t bytes) {
  if (heap_caps_get_largest_free_block(PNGC_CAPS) >= bytes) return true;
#ifdef PNGIMAGE_SPIRAM_FALLBACK
  if (heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) >= bytes) return true;
#endif
  return false;
}

// --- decoder glue -------------------------------------------------------------
// PNGdec takes plain function pointers, so the open file lives here. One
// decode at a time, from the main loop.
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

struct PngConvCtx {
  PNG* png;
  PngRowSink sink;
  void* user;
  uint32_t sinkUs;      // time spent in the sink
  int srcW, srcH, outW, outH;
  bool direct;          // native size: decoded rows go straight to the sink
  uint16_t* line;       // one source row as RGB565
  uint16_t* colOf;      // source column -> output column
  uint16_t* colCnt;     // source columns per output column
  uint32_t* acc;        // per output column: R5, G6, B5 sums
  uint16_t* outRow;     // one output row, high byte first
  int accRow;           // output row being accumulated, -1 when none
  int accRows;          // source rows accumulated into it
  int rowsOut;
  bool ok;
};

// Output index whose span holds the centre of source index i. Monotonic, and
// every output index receives at least one source index when out <= src.
static inline int pngConvMap(int i, int src, int out) {
  return (int)(((int64_t)(2 * i + 1) * out) / (2 * src));
}

static bool pngConvEmit(PngConvCtx* c, const uint16_t* row) {
  uint32_t t0 = micros();
  bool ok = c->sink(c->user, c->rowsOut, row, c->outW);
  c->sinkUs += micros() - t0;
  if (ok) c->rowsOut++;
  return ok;
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
  return pngConvEmit(c, c->outRow);
}

static int pngConvDraw(PNGDRAW* d) {
  PngConvCtx* c = (PngConvCtx*)d->pUser;
  if (!c->ok) return 0;

  if (c->direct) {
    // Alpha, if any, is blended onto black: the box background.
    c->png->getLineAsRGB565(d, c->line, PNG_RGB565_BIG_ENDIAN, 0x000000);
    if (!pngConvEmit(c, c->line)) { c->ok = false; return 0; }
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

// Output size for a source of w x h in a box of boxW x boxH (see the rule in
// PngImage.h).
static void pngPlanSize(int w, int h, int boxW, int boxH, int& outW, int& outH) {
  if (w <= boxW && h <= boxH) { outW = w; outH = h; return; }
  if ((int64_t)w * boxH > (int64_t)h * boxW) {
    outW = boxW;
    outH = (int)(((int64_t)h * boxW + w / 2) / w);
  } else {
    outH = boxH;
    outW = (int)(((int64_t)w * boxH + h / 2) / h);
  }
  if (outW < 1) outW = 1;
  if (outH < 1) outH = 1;
}

void pngFitSize(int w, int h, int boxW, int boxH, int* outW, int* outH) {
  int ow = 0, oh = 0;
  pngPlanSize(w, h, boxW, boxH, ow, oh);
  if (outW) *outW = ow;
  if (outH) *outH = oh;
}

bool pngIsSupported(fs::FS& fs, const char* pngPath) {
  // IHDR: bit depth at 24, interlace method at 28.
  File f = fs.open(pngPath, FILE_READ);
  if (!f) return false;
  uint8_t hd[29];
  bool ok = f.read(hd, 29) == 29 && hd[0] == 0x89 && hd[1] == 'P' &&
            hd[12] == 'I' && hd[13] == 'H' && hd[14] == 'D' && hd[15] == 'R';
  f.close();
  return ok && hd[24] <= 8 && hd[28] == 0;
}

bool pngReadHeader(fs::FS& fs, const char* pngPath, int* w, int* h, int* colorType) {
  // Signature, then the IHDR chunk: width at 16, height at 20, bit depth at
  // 24 and colour type at 25, all big-endian.
  File f = fs.open(pngPath, FILE_READ);
  if (!f) return false;
  uint8_t hd[26];
  bool ok = f.read(hd, 26) == 26 && hd[0] == 0x89 && hd[1] == 'P' && hd[2] == 'N' &&
            hd[3] == 'G' && hd[12] == 'I' && hd[13] == 'H' && hd[14] == 'D' && hd[15] == 'R';
  f.close();
  if (!ok) return false;
  if (w) *w = (int)((uint32_t)hd[16] << 24 | (uint32_t)hd[17] << 16 | hd[18] << 8 | hd[19]);
  if (h) *h = (int)((uint32_t)hd[20] << 24 | (uint32_t)hd[21] << 16 | hd[22] << 8 | hd[23]);
  if (colorType) *colorType = hd[25];
  return true;
}

bool decodePngScaled(fs::FS& fs, const char* pngPath, int boxW, int boxH,
                     PngSizeSink sizeSink, PngRowSink rowSink, void* user,
                     PngConvInfo* info) {
  // Not 'local': zutil.h, pulled in by PNGdec.h, defines it as a macro.
  PngConvInfo scratch;
  PngConvInfo& in = info ? *info : scratch;
  uint32_t keepSinkMs = in.sinkMs;     // convertPngTo565 adds its own share
  in = PngConvInfo();
  in.sinkMs = keepSinkMs;
  uint32_t t0 = millis();
  in.largestBlock = heap_caps_get_largest_free_block(PNGC_CAPS);

  if (!fs.exists(pngPath)) { in.error = "no such file"; return false; }

  // The header gives the size, and the size decides every other buffer.
  int w = 0, h = 0;
  if (!pngReadHeader(fs, pngPath, &w, &h, nullptr) ||
      w <= 0 || h <= 0 || w > 8192 || h > 8192) {
    in.error = "unsupported or damaged PNG";
    return false;
  }

  PngConvCtx c = {};
  c.sink = rowSink;
  c.user = user;
  c.srcW = in.srcW = w;
  c.srcH = in.srcH = h;
  c.accRow = -1;
  pngPlanSize(w, h, boxW, boxH, c.outW, c.outH);
  in.outW = c.outW;
  in.outH = c.outH;
  c.direct = (c.outW == c.srcW && c.outH == c.srcH);
  if (sizeSink && !sizeSink(user, c.outW, c.outH)) { in.error = "refused by the sink"; return false; }

  // Everything except the decoder first.
  bool ok = false;
  void* pngMem = nullptr;
  c.line = (uint16_t*)pngAlloc((size_t)c.srcW * 2);
  if (!c.direct) {
    c.colOf  = (uint16_t*)pngAlloc((size_t)c.srcW * 2);
    c.colCnt = (uint16_t*)pngCalloc(c.outW, 2);
    c.acc    = (uint32_t*)pngCalloc((size_t)c.outW * 3, 4);
    c.outRow = (uint16_t*)pngAlloc((size_t)c.outW * 2);
  }
  if (!c.line || (!c.direct && (!c.colOf || !c.colCnt || !c.acc || !c.outRow))) {
    in.error = "row buffer allocation failed";
  } else if (!pngHasBlock(sizeof(PNG) + 4096)) {
    in.error = "not enough contiguous heap";
  } else if (!(pngMem = pngAlloc(sizeof(PNG)))) {
    in.error = "PNG allocation failed";
  } else {
    c.png = new (pngMem) PNG();
    if (!c.direct) {
      for (int x = 0; x < c.srcW; x++) {
        c.colOf[x] = (uint16_t)pngConvMap(x, c.srcW, c.outW);
        c.colCnt[c.colOf[x]]++;
      }
    }
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
      if (rc != PNG_SUCCESS && c.ok) in.error = "decode failed";
      else if (!c.ok) in.error = "aborted by the sink";
      else if (c.rowsOut != c.outH) in.error = "incomplete output";
      else ok = true;
    }
    c.png->close();
    // Freed first, while nothing else has been released yet.
    c.png->~PNG();
    heap_caps_free(pngMem);
    c.png = nullptr;
  }

  heap_caps_free(c.outRow); heap_caps_free(c.acc); heap_caps_free(c.colCnt);
  heap_caps_free(c.colOf); heap_caps_free(c.line);
  in.sinkMs += c.sinkUs / 1000;
  in.ms = millis() - t0;
  return ok;
}

// --- file sink ----------------------------------------------------------------
// Output is written in blocks of this size: row-sized writes that do not line
// up with the card's sectors are several times slower.
static const size_t PNGC_WRITE_BLOCK = 4096;

struct PngFileSink {
  File out;
  uint8_t* buf;
  size_t len;
  size_t written;
};

static bool pngFileFlush(PngFileSink* s) {
  if (!s->len) return true;
  bool ok = (s->out.write(s->buf, s->len) == s->len);
  s->written += s->len;
  s->len = 0;
  return ok;
}

static bool pngFileSize(void* user, int outW, int outH) {
  PngFileSink* s = (PngFileSink*)user;
  uint8_t head[4] = { (uint8_t)(outW & 0xFF), (uint8_t)(outW >> 8),
                      (uint8_t)(outH & 0xFF), (uint8_t)(outH >> 8) };
  return s->out.write(head, 4) == 4;
}

static bool pngFileRow(void* user, int, const uint16_t* row, int w) {
  PngFileSink* s = (PngFileSink*)user;
  const uint8_t* data = (const uint8_t*)row;
  size_t len = (size_t)w * 2;
  while (len) {
    size_t n = PNGC_WRITE_BLOCK - s->len;
    if (n > len) n = len;
    memcpy(s->buf + s->len, data, n);
    s->len += n; data += n; len -= n;
    if (s->len == PNGC_WRITE_BLOCK && !pngFileFlush(s)) return false;
  }
  return true;
}

bool convertPngTo565(fs::FS& fs, const char* pngPath, const char* rawPath,
                     int boxW, int boxH, PngConvInfo* info) {
  PngConvInfo scratch;
  PngConvInfo& in = info ? *info : scratch;
  in = PngConvInfo();
  uint32_t t0 = millis();

  // File and block buffer before the decoder: see the allocation note in
  // PngImage.h.
  String tmpPath = String(rawPath) + ".tmp";
  PngFileSink s = {};
  s.buf = (uint8_t*)pngAlloc(PNGC_WRITE_BLOCK);
  if (!s.buf) { in.error = "write buffer allocation failed"; return false; }
  if (fs.exists(tmpPath)) fs.remove(tmpPath);
  s.out = fs.open(tmpPath, FILE_WRITE);
  bool ok = false;
  if (!s.out) {
    in.error = "cannot write output";
  } else {
    ok = decodePngScaled(fs, pngPath, boxW, boxH, pngFileSize, pngFileRow, &s, &in);
    uint32_t tw = millis();
    if (ok && !pngFileFlush(&s)) { ok = false; in.error = "write failed"; }
    if (ok && s.written != (size_t)in.outW * in.outH * 2) { ok = false; in.error = "incomplete output"; }
    s.out.close();
    in.sinkMs += millis() - tw;
  }
  heap_caps_free(s.buf);

  if (ok) {
    if (fs.exists(rawPath)) fs.remove(rawPath);
    ok = fs.rename(tmpPath.c_str(), rawPath);
    if (!ok) in.error = "rename failed";
  }
  if (!ok && fs.exists(tmpPath)) fs.remove(tmpPath);
  in.ms = millis() - t0;
  return ok;
}
