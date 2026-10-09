// MiSTer Monitor
// Copyright (C) 2025-2026 chipster6502
// SPDX-License-Identifier: GPL-3.0-or-later

// =============================================================================
// FirmwareUpdate.h  —  over-the-air firmware update, served by the MiSTer
// =============================================================================
// The release workflow ships one app image per board, plus a manifest, inside
// the Downloader database, next to the server:
//
//     Scripts/.config/mister_monitor/firmware/manifest.json
//     Scripts/.config/mister_monitor/firmware/<board>.bin
//
// so an Update All on the MiSTer leaves both halves of a release on the card
// together. The server exposes them as /firmware/manifest and
// /firmware/<file>, and this module closes the loop: when the server reports
// a version newer than FIRMWARE_VERSION, it fetches the manifest, looks up
// this board, and can download the image straight into the spare OTA slot.
//
// Three ways in:
//   * the sketch, on screen: firmwareCheckOffer() after the snapshot reveals a
//     newer server, then firmwareApplyOffer() on a tap or with [update] ota_auto
//   * POST /update on the device web server: same path, just started from a
//     browser; the sketch runs it from loop() when firmwareWebRequested is set
//   * POST /update/upload: a .bin chosen in the browser, for a display that
//     has no reachable MiSTer at all (or for testing a build before release)
//
// What makes this safe to offer without a cable nearby:
//   * The image goes to the OTA slot that is NOT running. Update.end()
//     verifies the ESP image checksum and SHA-256, and only then points
//     otadata at the new slot. A download that stops short, or a corrupted
//     one, never boots; the running firmware stays exactly where it is.
//   * The manifest carries size and MD5 computed by CI, so the download is
//     also checked against what was built, not just against itself.
//   * Boards flashed with a single-slot layout (the CYD images before
//     min_spiffs) are told to reflash once by cable instead of being offered
//     an update that Update.begin() would refuse anyway.
//
// Integration (per board):
//   #define FIRMWARE_VERSION "x.y.z"    (already the sketch's)
//   #define FIRMWARE_BOARD_ID "cyd_ili9341"   — manifest key for this build
//   #include "WebConfig.h"               (for webConfigAuthOk)
//   #include "FirmwareUpdate.h"
//   registerFirmwareUpdateRoutes() next to registerWebConfigRoutes().
//   In getStateSnapshot(): firmwareCheckOffer() when server_version is newer.
//   In loop(): if (firmwareWebRequested) run the on-screen update.
//   The sketch owns every pixel: it passes a progress callback and draws.
//
// config.ini keys (parsed in AppConfig.h):
//   [update] ota_auto = true / false — install without a tap when an update is
//                                   offered (default false)
//
// This file is byte-identical across all board folders.
// =============================================================================

#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <Update.h>
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "AppConfig.h"

#ifndef FIRMWARE_VERSION
#error "FirmwareUpdate.h needs FIRMWARE_VERSION defined by the sketch"
#endif
#ifndef FIRMWARE_BOARD_ID
#error "FirmwareUpdate.h needs FIRMWARE_BOARD_ID defined by the sketch"
#endif

extern WebServer   screenshotServer;   // defined in the sketch (port 8080)
extern AppConfig   appConfig;          // defined in the sketch, loaded at boot
extern const char* misterIP;           // defined in the sketch; "" when unknown

// -----------------------------------------------------------------------------
// What the server has for this board, as learned from /firmware/manifest.
// -----------------------------------------------------------------------------
struct FirmwareOffer {
  bool   checked   = false;   // manifest fetched (successfully or not)
  bool   available = false;   // an image newer than FIRMWARE_VERSION exists
  bool   declined  = false;   // the on-screen banner ran out without a tap
  bool   failed    = false;   // an install attempt failed this boot
  String version;             // manifest version
  String file;                // image name on the server
  size_t size = 0;            // bytes, from the manifest
  String md5;                 // hex digest, from the manifest
  String reason;              // why !available, or the last error — for logs and /update
};
FirmwareOffer firmwareOffer;

// Set by POST /update; the sketch runs the update from loop() where it can
// draw. A web handler cannot: handleClient() is pumped from inside drawing
// and download code, so starting a reflash there would re-enter all of it.
bool firmwareWebRequested = false;

// Progress sink supplied by the sketch. phase is a short label to print,
// total is 0 when unknown (browser upload: the size arrives at the end).
typedef void (*FirmwareProgressFn)(const char* phase, size_t done, size_t total);
static FirmwareProgressFn fwProgressCb = nullptr;

// -----------------------------------------------------------------------------
// Version strings: "a.b.c" compared numerically, anything past the third
// number ignored ("2.12.0-rc1" sorts as 2.12.0). Missing parts count as 0.
// -----------------------------------------------------------------------------
static void fwParseVersion(const String& v, int out[3]) {
  out[0] = out[1] = out[2] = 0;
  int part = 0, pos = 0;
  while (part < 3 && pos < (int)v.length()) {
    int val = 0; bool any = false;
    while (pos < (int)v.length() && isDigit(v[pos])) { val = val * 10 + (v[pos] - '0'); pos++; any = true; }
    if (!any) break;
    out[part++] = val;
    if (pos < (int)v.length() && v[pos] == '.') pos++; else break;
  }
}

static int fwCompareVersions(const String& a, const String& b) {
  int va[3], vb[3];
  fwParseVersion(a, va);
  fwParseVersion(b, vb);
  for (int i = 0; i < 3; i++) {
    if (va[i] != vb[i]) return va[i] < vb[i] ? -1 : 1;
  }
  return 0;
}

// -----------------------------------------------------------------------------
// Partition layout: an update needs an OTA slot other than the running one.
// Update.begin() checks the same thing, but by then a download has happened;
// asking first lets the sketch explain instead of failing late.
// -----------------------------------------------------------------------------
inline bool firmwareOtaCapable() {
  const esp_partition_t* next    = esp_ota_get_next_update_partition(NULL);
  const esp_partition_t* running = esp_ota_get_running_partition();
  return next != NULL && next != running;
}

// Size of the slot an update would be written to (0 when none).
inline size_t firmwareOtaSlotSize() {
  if (!firmwareOtaCapable()) return 0;
  return esp_ota_get_next_update_partition(NULL)->size;
}

// -----------------------------------------------------------------------------
// Minimal JSON lookups for the manifest. CI writes it with json.dump, so the
// shape is fixed:
//   {"version": "x.y.z", "boards": {"<id>": {"file": "...", "size": N, "md5": "..."}, ...}}
// fwJsonValue() returns the raw token after "key": with quotes stripped, or
// "" when the key is absent. Searching starts at `from`, which is how the
// per-board object is isolated: find the board key, then look for fields
// only between its '{' and the matching '}'.
// -----------------------------------------------------------------------------
static String fwJsonValue(const String& json, const char* key, int from, int to) {
  String needle = String("\"") + key + "\"";
  int k = json.indexOf(needle, from);
  if (k < 0 || k >= to) return "";
  int colon = json.indexOf(':', k + needle.length());
  if (colon < 0 || colon >= to) return "";
  int p = colon + 1;
  while (p < to && (json[p] == ' ' || json[p] == '\n' || json[p] == '\r' || json[p] == '\t')) p++;
  if (p >= to) return "";
  if (json[p] == '"') {
    int q = json.indexOf('"', p + 1);
    if (q < 0 || q > to) return "";
    return json.substring(p + 1, q);
  }
  int e = p;
  while (e < to && json[e] != ',' && json[e] != '}' && json[e] != ' ' && json[e] != '\n' && json[e] != '\r') e++;
  return json.substring(p, e);
}

// -----------------------------------------------------------------------------
// Fetch /firmware/manifest and fill firmwareOffer for FIRMWARE_BOARD_ID.
// Returns true when an update is available. Cheap (one small GET), but it is
// a network call, so the sketch should call it from the same place it already
// talks to the server, not from a draw path.
// -----------------------------------------------------------------------------
inline bool firmwareCheckOffer() {
  FirmwareOffer o;
  o.checked = true;

  if (strlen(misterIP) == 0) {
    o.reason = "MiSTer address unknown";
    firmwareOffer = o;
    return false;
  }

  // The client must outlive the HTTPClient: ~HTTPClient() calls stop() on it
  // whenever the server closed the connection first (it does: HTTP/1.0), and
  // a client destroyed before that is a use-after-free inside lwIP.
  WiFiClient client;
  HTTPClient http;
  http.setTimeout(5000);
  String url = String("http://") + misterIP + ":8081/firmware/manifest";
  if (!http.begin(client, url)) {
    o.reason = "manifest request failed";
    firmwareOffer = o;
    return false;
  }
  int code = http.GET();
  if (code != 200) {
    // 404 is the normal answer from a server that shipped before OTA existed
    // or whose Downloader run has not pulled the images yet.
    o.reason = (code == 404) ? "no firmware images on the MiSTer"
                             : "manifest HTTP " + String(code);
    http.end();
    firmwareOffer = o;
    Serial.printf("[OTA] %s\n", o.reason.c_str());
    return false;
  }
  String json = http.getString();
  http.end();

  o.version = fwJsonValue(json, "version", 0, json.length());
  o.version.trim();

  String key = String("\"") + FIRMWARE_BOARD_ID + "\"";
  int k = json.indexOf(key);
  int open  = (k >= 0) ? json.indexOf('{', k) : -1;
  int close = (open >= 0) ? json.indexOf('}', open) : -1;
  if (close < 0) {
    o.reason = String("no image for ") + FIRMWARE_BOARD_ID + " in manifest " + o.version;
    firmwareOffer = o;
    Serial.printf("[OTA] %s\n", o.reason.c_str());
    return false;
  }
  o.file = fwJsonValue(json, "file", open, close);
  o.size = (size_t)fwJsonValue(json, "size", open, close).toInt();
  o.md5  = fwJsonValue(json, "md5",  open, close);
  o.md5.toLowerCase();

  int cmp = fwCompareVersions(o.version, FIRMWARE_VERSION);
  if (cmp <= 0 || o.file.length() == 0 || o.size == 0) {
    o.reason = (cmp == 0) ? "server has the same version"
             : (cmp <  0) ? "server images are older than this firmware"
                          : "manifest entry incomplete";
    firmwareOffer = o;
    Serial.printf("[OTA] %s (manifest %s, firmware %s)\n",
                  o.reason.c_str(), o.version.c_str(), FIRMWARE_VERSION);
    return false;
  }

  o.available = true;
  firmwareOffer = o;
  Serial.printf("[OTA] Update available: %s -> %s (%s, %u bytes)\n",
                FIRMWARE_VERSION, o.version.c_str(), o.file.c_str(), (unsigned)o.size);
  return true;
}

// -----------------------------------------------------------------------------
// Download the offered image and write it to the spare slot. Blocking: the
// sketch draws progress through the callback and nothing else runs. On
// success the new image is already selected for the next boot; the caller
// decides when to ESP.restart(). On failure firmwareOffer.reason says why
// and the running firmware is untouched.
// -----------------------------------------------------------------------------
static void fwUpdateProgress(size_t done, size_t total) {
  if (fwProgressCb) fwProgressCb("Writing", done, total);
}

inline bool firmwareApplyOffer(FirmwareProgressFn progress) {
  fwProgressCb = progress;
  FirmwareOffer& o = firmwareOffer;

  if (!o.available) {
    o.reason = "no update offered";
    return false;
  }
  if (!firmwareOtaCapable()) {
    o.failed = true;
    o.reason = "single app slot: reflash once with the web flasher to enable updates";
    Serial.printf("[OTA] %s\n", o.reason.c_str());
    return false;
  }
  size_t slot = firmwareOtaSlotSize();
  if (o.size > slot) {
    o.failed = true;
    o.reason = "image (" + String((unsigned)o.size) + " B) larger than the OTA slot (" +
               String((unsigned)slot) + " B)";
    Serial.printf("[OTA] %s\n", o.reason.c_str());
    return false;
  }

  if (fwProgressCb) fwProgressCb("Connecting", 0, o.size);

  WiFiClient client;        // declared first: must outlive http (see firmwareCheckOffer)
  HTTPClient http;
  http.setTimeout(15000);   // also the per-read stall limit inside writeStream
  String url = String("http://") + misterIP + ":8081/firmware/" + o.file;
  Serial.printf("[OTA] GET %s\n", url.c_str());
  if (!http.begin(client, url)) {
    o.failed = true; o.reason = "download request failed";
    return false;
  }
  int code = http.GET();
  if (code != 200) {
    o.failed = true; o.reason = "download HTTP " + String(code);
    http.end();
    Serial.printf("[OTA] %s\n", o.reason.c_str());
    return false;
  }
  int len = http.getSize();
  if (len <= 0 || (size_t)len != o.size) {
    o.failed = true;
    o.reason = "server sent " + String(len) + " bytes, manifest says " + String((unsigned)o.size);
    http.end();
    Serial.printf("[OTA] %s\n", o.reason.c_str());
    return false;
  }

  // Look at the first byte before touching flash. An ESP app image starts
  // with 0xE9; the merged flasher image starts with 0xFF padding (the
  // bootloader sits at 0x1000), and Update would report that as a
  // "decryption error", which helps nobody. Say what the file is instead.
  WiFiClient* stream = http.getStreamPtr();
  {
    unsigned long t0 = millis();
    while (!stream->available() && stream->connected() && millis() - t0 < 5000) delay(10);
    int first = stream->peek();
    if (first != 0xE9) {
      o.failed = true;
      o.reason = (first < 0) ? String("no data from the server")
               : String("not an app image (first byte 0x") + String(first, HEX) +
                 "): the OTA file must be the .ino.bin, not the merged flasher image";
      http.end();
      Serial.printf("[OTA] %s\n", o.reason.c_str());
      return false;
    }
  }

  // Boards with PSRAM download the whole image there first and write the
  // flash afterwards in one burst. On the MIPI-DSI panels the picture is
  // scanned out of a PSRAM framebuffer, and every flash erase/program turns
  // that path off for a moment, so writing while downloading makes the panel
  // flash its idle colour for the whole transfer. Buffered, the download
  // shows a clean progress bar and the sketch can blank the panel for the
  // few seconds of "Flashing". Boards without PSRAM stream as before.
  uint8_t* buf = psramFound() ? (uint8_t*)ps_malloc((size_t)len) : nullptr;
  const bool buffered = (buf != nullptr);
  if (buffered) {
    size_t got = 0;
    unsigned long lastData = millis();
    while (got < (size_t)len) {
      size_t avail = stream->available();
      if (avail) {
        size_t want = (size_t)len - got;
        size_t n = stream->readBytes(buf + got, avail < want ? avail : want);
        got += n;
        lastData = millis();
        if (fwProgressCb) fwProgressCb("Downloading", got, (size_t)len);
      } else if (!stream->connected() || millis() - lastData > 15000) {
        break;
      } else {
        delay(1);
      }
    }
    http.end();
    if (got != (size_t)len) {
      free(buf);
      o.failed = true;
      o.reason = "short download: " + String((unsigned)got) + " of " + String(len);
      Serial.printf("[OTA] %s\n", o.reason.c_str());
      return false;
    }
  }

  if (!Update.begin((size_t)len)) {
    o.failed = true; o.reason = String("Update.begin: ") + Update.errorString();
    if (buffered) free(buf); else http.end();
    Serial.printf("[OTA] %s\n", o.reason.c_str());
    return false;
  }
  if (o.md5.length() == 32) Update.setMD5(o.md5.c_str());

  size_t written = 0;
  if (buffered) {
    if (fwProgressCb) fwProgressCb("Flashing", 0, (size_t)len);
    Update.onProgress(nullptr);
    // Chunked so the other tasks get a turn between flash bursts.
    const size_t CHUNK = 32 * 1024;
    while (written < (size_t)len) {
      size_t n = (size_t)len - written;
      if (n > CHUNK) n = CHUNK;
      size_t w = Update.write(buf + written, n);
      written += w;
      if (w != n) break;
      delay(1);
    }
    free(buf);
  } else {
    Update.onProgress(fwUpdateProgress);
    written = Update.writeStream(*stream);
    http.end();
  }

  if (written != (size_t)len) {
    o.failed = true;
    o.reason = (buffered ? String("flash write: ") : String("short download: ")) +
               String((unsigned)written) + " of " + String(len) +
               " (" + Update.errorString() + ")";
    Update.abort();
    Serial.printf("[OTA] %s\n", o.reason.c_str());
    return false;
  }

  if (fwProgressCb) fwProgressCb("Verifying", written, written);
  if (!Update.end()) {
    o.failed = true; o.reason = String("Update.end: ") + Update.errorString();
    Serial.printf("[OTA] %s\n", o.reason.c_str());
    return false;
  }

  Serial.printf("[OTA] Image %s written and verified; next boot runs %s\n",
                o.version.c_str(), o.version.c_str());
  return true;
}

// =============================================================================
// Web side: GET /update (status + buttons), POST /update (install from the
// MiSTer, run by the sketch), POST /update/upload (a .bin from the browser).
// Same auth gate as /config: webConfigAuthOk() from WebConfig.h.
// =============================================================================
static const char FWUPDATE_STYLE[] PROGMEM = R"rawliteral(<style>
body{background:#000;color:#0FF;font-family:monospace;padding:16px}
a{color:#0FF} h1{margin-top:0} .box{border:1px solid #0FF;padding:12px;margin:12px 0;max-width:640px}
.ok{color:#5F5} .warn{color:#FF5} .err{color:#F55} .dim{color:#888;font-size:12px}
button{background:#0FF;color:#000;border:0;padding:8px 20px;font-family:monospace;font-weight:bold;cursor:pointer}
input[type=file]{color:#0FF}
</style>)rawliteral";

static String fwHtmlEscape(const String& s) {
  String out; out.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if      (c == '&') out += "&amp;";
    else if (c == '<') out += "&lt;";
    else if (c == '>') out += "&gt;";
    else               out += c;
  }
  return out;
}

static void handleFirmwareUpdateGet() {
  if (!webConfigAuthOk()) return;

  // Ask the server now: the page is the one place a user comes to find out,
  // and the answer may have changed since the last snapshot poll.
  firmwareCheckOffer();
  const FirmwareOffer& o = firmwareOffer;

  String html;
  html.reserve(2600);
  html += F("<!DOCTYPE html><html><head><meta charset=\"utf-8\"><title>MiSTer Monitor - firmware update</title>");
  html += FPSTR(FWUPDATE_STYLE);
  html += F("</head><body><h1>Firmware update</h1>");

  html += F("<div class=\"box\"><p>Board: <b>");
  html += FIRMWARE_BOARD_ID;
  html += F("</b><br>Running: <b>");
  html += FIRMWARE_VERSION;
  html += F("</b><br>On the MiSTer: <b>");
  html += o.version.length() ? fwHtmlEscape(o.version) : String("?");
  html += F("</b></p>");

  if (!firmwareOtaCapable()) {
    html += F("<p class=\"err\">This display was flashed with a single app slot, so it cannot take an "
              "over-the-air update yet. Flash it once more with the web flasher (the current release "
              "uses a two-slot layout); after that, updates arrive here.</p>");
  } else if (o.available) {
    html += F("<p class=\"ok\">Update to <b>");
    html += fwHtmlEscape(o.version);
    html += F("</b> available (");
    html += String((unsigned)(o.size / 1024));
    html += F(" KB).</p><form method=\"POST\" action=\"/update\"><button type=\"submit\">Install from the MiSTer</button></form>"
              "<p class=\"dim\">The display shows the progress and restarts by itself. Nothing on the microSD changes.</p>");
  } else {
    html += F("<p class=\"warn\">");
    html += fwHtmlEscape(o.reason);
    html += F("</p><p class=\"dim\">Images arrive with the server: run Update All or Downloader on the MiSTer, "
              "then reload this page.</p>");
  }
  html += F("</div>");

  if (firmwareOtaCapable()) {
    html += F("<div class=\"box\"><p>Or install an image from this computer, for a display that cannot reach "
              "the MiSTer or to try a build before release. Use the app image for <b>");
    html += FIRMWARE_BOARD_ID;
    html += F("</b> (the <code>.ino.bin</code> from the Arduino build, <i>not</i> the merged flasher image).</p>"
              "<form method=\"POST\" action=\"/update/upload\" enctype=\"multipart/form-data\">"
              "<input type=\"file\" name=\"firmware\" accept=\".bin\"> <button type=\"submit\">Upload and install</button></form>"
              "<p class=\"dim\">Slot size: ");
    html += String((unsigned)(firmwareOtaSlotSize() / 1024));
    html += F(" KB. A wrong or corrupted image is rejected before it can boot.</p></div>");
  }

  html += F("<p><a href=\"/config\">config.ini</a> &middot; <a href=\"/files\">SD card</a></p></body></html>");
  screenshotServer.send(200, "text/html", html);
}

static const char FWUPDATE_STARTED_PAGE[] PROGMEM = R"rawliteral(<!DOCTYPE html><html><head>
<meta charset="utf-8"><meta http-equiv="refresh" content="60;url=/update">
<title>MiSTer Monitor - updating</title></head><body
style="background:#000;color:#0FF;font-family:monospace;padding:16px">
<h1>Updating&hellip;</h1>
<p>The display is downloading the new firmware from the MiSTer and will restart when it is written.
This page reloads in 60 seconds; the version shown then is the one running.</p>
</body></html>)rawliteral";

static void handleFirmwareUpdatePost() {
  if (!webConfigAuthOk()) return;
  if (!firmwareOffer.available) firmwareCheckOffer();
  if (!firmwareOffer.available) {
    screenshotServer.send(409, "text/plain", "No update available: " + firmwareOffer.reason);
    return;
  }
  firmwareWebRequested = true;   // picked up by loop(), which can draw
  Serial.println("[OTA] Update requested via /update");
  screenshotServer.send_P(200, "text/html", FWUPDATE_STARTED_PAGE);
}

// POST /update/upload — the browser streams a .bin; chunks go straight to
// the spare slot. Nothing is kept in RAM beyond WebServer's own chunk.
static String fwUploadError;
static bool   fwUploadOk = false;

static void handleFirmwareUploadChunk() {
  HTTPUpload& up = screenshotServer.upload();
  switch (up.status) {
    case UPLOAD_FILE_START:
      fwUploadError = "";
      fwUploadOk = false;
      if (appConfig.webPassword.length() > 0 &&
          !screenshotServer.authenticate("admin", appConfig.webPassword.c_str())) {
        fwUploadError = F("Authentication required");
        return;
      }
      if (!firmwareOtaCapable()) {
        fwUploadError = F("Single app slot: reflash once with the web flasher to enable updates");
        return;
      }
      Serial.printf("[OTA] Browser upload start: %s\n", up.filename.c_str());
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
        fwUploadError = String("Update.begin: ") + Update.errorString();
        return;
      }
      if (fwProgressCb) fwProgressCb("Receiving", 0, 0);
      break;

    case UPLOAD_FILE_WRITE:
      if (fwUploadError.length() == 0 && Update.isRunning()) {
        if (up.totalSize == 0 && up.currentSize > 0 && up.buf[0] != 0xE9) {
          // Same check as the MiSTer path: refuse anything that is not an
          // app image before Update turns it into a "decryption error".
          fwUploadError = String("not an app image (first byte 0x") + String(up.buf[0], HEX) +
                          "): upload the .ino.bin, not the merged flasher image";
          Update.abort();
          return;
        }
        if (Update.write(up.buf, up.currentSize) != up.currentSize) {
          fwUploadError = String("write: ") + Update.errorString();
          Update.abort();
        } else if (fwProgressCb) {
          fwProgressCb("Receiving", up.totalSize + up.currentSize, 0);
        }
      }
      break;

    case UPLOAD_FILE_END:
      if (fwUploadError.length() == 0 && Update.isRunning()) {
        if (fwProgressCb) fwProgressCb("Verifying", up.totalSize, up.totalSize);
        if (Update.end(true)) {
          fwUploadOk = true;
          Serial.printf("[OTA] Browser upload written and verified (%u bytes)\n", (unsigned)up.totalSize);
        } else {
          fwUploadError = String("Update.end: ") + Update.errorString();
        }
      }
      break;

    case UPLOAD_FILE_ABORTED:
      if (Update.isRunning()) Update.abort();
      fwUploadError = F("Upload aborted");
      break;
  }
}

static const char FWUPDATE_UPLOADED_PAGE[] PROGMEM = R"rawliteral(<!DOCTYPE html><html><head>
<meta charset="utf-8"><meta http-equiv="refresh" content="20;url=/update">
<title>MiSTer Monitor - restarting</title></head><body
style="background:#000;color:#0FF;font-family:monospace;padding:16px">
<h1>Firmware written</h1>
<p>The image was verified and the display is restarting into it. This page reloads in 20 seconds.</p>
</body></html>)rawliteral";

static void handleFirmwareUploadDone() {
  if (!webConfigAuthOk()) return;
  if (fwUploadError.length() > 0 || !fwUploadOk) {
    Serial.printf("[OTA] Browser upload failed: %s\n", fwUploadError.c_str());
    screenshotServer.send(500, "text/plain", fwUploadError.length() ? fwUploadError : String("Upload failed"));
    if (fwProgressCb) fwProgressCb("Failed", 0, 0);
    return;
  }
  screenshotServer.send_P(200, "text/html", FWUPDATE_UPLOADED_PAGE);
  if (fwProgressCb) fwProgressCb("Restarting", 0, 0);
  delay(500);
  ESP.restart();
}

// Registered next to the /config routes; obeys the same [ui] web_config switch.
// The progress callback is what the sketch would draw for an on-screen update,
// so a browser upload shows on the panel too.
inline void registerFirmwareUpdateRoutes(FirmwareProgressFn progress) {
  if (!appConfig.webConfig) return;   // WebConfig already logged the reason
  fwProgressCb = progress;
  screenshotServer.on("/update",        HTTP_GET,  handleFirmwareUpdateGet);
  screenshotServer.on("/update",        HTTP_POST, handleFirmwareUpdatePost);
  screenshotServer.on("/update/upload", HTTP_POST, handleFirmwareUploadDone, handleFirmwareUploadChunk);
  Serial.printf("[OTA] Board %s, firmware %s, OTA slot %s; page at http://%s:8080/update\n",
                FIRMWARE_BOARD_ID, FIRMWARE_VERSION,
                firmwareOtaCapable() ? "ready" : "MISSING (single-slot layout)",
                WiFi.localIP().toString().c_str());
}
