// MiSTer Monitor
// Copyright (C) 2025-2026 chipster6502
// SPDX-License-Identifier: GPL-3.0-or-later

// =============================================================================
// WebFiles.h  —  SD card file browser on the device web server (port 8080)
// =============================================================================
// Routes (all share the [ui] web_config switch and web_password auth gate
// defined in WebConfig.h):
//
//   GET  /files?dir=/path        — directory listing with breadcrumb
//   GET  /files/download?path=…  — stream a file (view .bak, artwork, logs)
//   POST /files/delete           — delete a file or an EMPTY directory
//   POST /files/mkdir            — create a directory
//   POST /files/upload?dir=…     — multipart upload into the current directory
//
// Scope decisions (v1):
//   * Deletion covers files and empty directories only. Recursive directory
//     deletion is deliberately out: more code, and one misclick could wipe an
//     artwork tree that took hours of ScreenScraper quota to build.
//   * Uploads overwrite an existing file of the same name — that is the
//     custom-artwork use case (replace a badly scraped image).
//   * Progressive JPEGs are rejected after upload with a clear message:
//     JPEGDEC only decodes baseline JPEG, so accepting them would just show
//     a black hole on screen and generate a support issue. The check walks
//     the real JPEG marker chain (SOF0/SOF1 = baseline, SOF2 = progressive)
//     on the written file, so EXIF blocks of any size cannot fool it.
//   * Listings are streamed in FAT enumeration order, unsorted: collecting
//     hundreds of artwork filenames to sort them would fragment the heap on
//     the CYD boards, and handleClient() also runs mid-download (pumpedDelay).
//   * Path hygiene: every path from the client must be absolute, without
//     "..", without backslashes, and within a length cap. This is a LAN
//     device, but path traversal is the one input worth being strict about.
//
// Uploads block the display UI for their duration (WebServer processes a
// request to completion inside one handleClient() call) — same behaviour the
// screenshot BMP capture already has.
//
// This file is byte-identical across all board folders, like WebConfig.h.
// =============================================================================

#pragma once
#include <Arduino.h>
#include <SD.h>
#include <WiFi.h>
#include <WebServer.h>
#include "AppConfig.h"
#include "WebConfig.h"   // webConfigAuthOk() + shared [ui] web_config gate

static const size_t WEBFILES_MAX_PATH = 200;

// -----------------------------------------------------------------------------
// Path and text helpers
// -----------------------------------------------------------------------------
static bool webFilesPathOk(const String& p) {
  if (p.length() == 0 || p.length() > WEBFILES_MAX_PATH) return false;
  if (p[0] != '/')            return false;
  if (p.indexOf("..") >= 0)   return false;
  if (p.indexOf('\\') >= 0)   return false;
  return true;
}

// Escape for HTML text and attribute values (quotes included: file names end
// up inside value="..." of the delete forms).
static String webFilesHtmlEscape(const String& s) {
  String out;
  out.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if      (c == '&')  out += F("&amp;");
    else if (c == '<')  out += F("&lt;");
    else if (c == '>')  out += F("&gt;");
    else if (c == '"')  out += F("&quot;");
    else                out += c;
  }
  return out;
}

// Percent-encode for use inside query string values. '/' is left readable —
// it is valid there and keeps ?dir=/cores/S legible.
static String webFilesUrlEncode(const String& s) {
  static const char* hex = "0123456789ABCDEF";
  String out;
  out.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' ||
        c == '~' || c == '/') {
      out += c;
    } else {
      out += '%';
      out += hex[(c >> 4) & 0x0F];
      out += hex[c & 0x0F];
    }
  }
  return out;
}

static String webFilesParentDir(const String& path) {
  int slash = path.lastIndexOf('/');
  if (slash <= 0) return "/";
  return path.substring(0, slash);
}

static void webFilesRedirectToDir(const String& dir) {
  screenshotServer.sendHeader("Location", "/files?dir=" + webFilesUrlEncode(dir));
  screenshotServer.send(303, "text/plain", "");
}

static bool webFilesIsJpegName(const String& path) {
  String p = path;
  p.toLowerCase();
  return p.endsWith(".jpg") || p.endsWith(".jpeg");
}

// -----------------------------------------------------------------------------
// Progressive-JPEG detection: walk the marker chain of the written file.
// SOF0 (0xC0) / SOF1 (0xC1) = baseline/extended sequential -> accepted.
// SOF2 (0xC2) = progressive -> rejected. Stops at SOS (0xDA) or after a
// sanity cap of 64 segments. Any malformed structure is treated as "not
// progressive" — JPEGDEC will produce its own error path for those.
// -----------------------------------------------------------------------------
static bool webFilesJpegIsProgressive(const String& path) {
  File f = SD.open(path, FILE_READ);
  if (!f) return false;
  bool progressive = false;
  uint8_t b2[2];
  if (f.read(b2, 2) == 2 && b2[0] == 0xFF && b2[1] == 0xD8) {
    for (int seg = 0; seg < 64; seg++) {
      uint8_t m;
      if (f.read(&m, 1) != 1) break;
      if (m != 0xFF) continue;                 // resync on stray byte
      uint8_t code = 0xFF;
      while (code == 0xFF) {                   // skip fill bytes
        if (f.read(&code, 1) != 1) { code = 0; break; }
      }
      if (code == 0) break;
      if (code == 0xC2) { progressive = true; break; }
      if (code == 0xC0 || code == 0xC1 || code == 0xDA) break;
      if (code == 0x01 || (code >= 0xD0 && code <= 0xD9)) continue;  // no length
      if (f.read(b2, 2) != 2) break;
      uint16_t len = ((uint16_t)b2[0] << 8) | b2[1];
      if (len < 2) break;
      if (!f.seek(f.position() + (len - 2))) break;
    }
  }
  f.close();
  return progressive;
}

// -----------------------------------------------------------------------------
// HTML
// -----------------------------------------------------------------------------
static const char WEBFILES_PAGE_HEAD[] PROGMEM = R"rawliteral(<!DOCTYPE html><html><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>MiSTer Monitor - SD card</title>
<style>
body{background:#000;color:#0FF;font-family:monospace;margin:0;padding:16px}
h1{font-size:18px;margin:0 0 12px}
a{color:#0FF}
table{border-collapse:collapse;margin-top:12px}
td{padding:2px 14px 2px 0;vertical-align:middle}
button{background:#0FF;color:#000;border:0;padding:4px 12px;font-family:monospace;
font-weight:bold;cursor:pointer}
.del{background:#F55;padding:2px 8px}
.dl{text-decoration:none;font-size:16px}
form{display:inline}
input[type=text]{background:#111;color:#0F0;border:1px solid #0FF;
font-family:monospace;padding:4px}
input[type=file]{color:#0FF;font-family:monospace}
p.note{color:#888;font-size:12px}
.crumb{margin-bottom:8px}
</style></head><body>
<h1>MiSTer Monitor &mdash; SD card</h1>)rawliteral";

static const char WEBFILES_PAGE_TAIL[] PROGMEM = R"rawliteral(</table>
<p class="note">Click a name to preview it, or the arrow to download. Deletion
covers files and empty directories. Uploads overwrite an
existing file of the same name. Artwork must be baseline JPEG &mdash; progressive
JPEGs are rejected.</p>
<p><a href="/config">Edit config.ini</a></p>
</body></html>)rawliteral";

// -----------------------------------------------------------------------------
// GET /files — streamed directory listing
// -----------------------------------------------------------------------------
static void handleWebFilesList() {
  if (!webConfigAuthOk()) return;

  String dir = screenshotServer.hasArg("dir") ? screenshotServer.arg("dir") : "/";
  while (dir.length() > 1 && dir.endsWith("/")) dir.remove(dir.length() - 1);
  if (!webFilesPathOk(dir)) {
    screenshotServer.send(400, "text/plain", "Invalid directory path");
    return;
  }

  File d = SD.open(dir);
  if (!d || !d.isDirectory()) {
    if (d) d.close();
    screenshotServer.send(404, "text/plain", "Not a directory");
    return;
  }

  screenshotServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
  screenshotServer.send(200, "text/html", "");
  screenshotServer.sendContent_P(WEBFILES_PAGE_HEAD);

  // Breadcrumb: / cores / S
  {
    String crumb = F("<div class=\"crumb\"><a href=\"/files?dir=/\">[root]</a>");
    if (dir != "/") {
      String acc = "";
      int start = 1;
      while (start <= (int)dir.length()) {
        int slash = dir.indexOf('/', start);
        if (slash < 0) slash = dir.length();
        String part = dir.substring(start, slash);
        acc += "/" + part;
        crumb += " / <a href=\"/files?dir=" + webFilesUrlEncode(acc) + "\">" +
                 webFilesHtmlEscape(part) + "</a>";
        start = slash + 1;
      }
    }
    crumb += F("</div>");
    screenshotServer.sendContent(crumb);
  }

  // Action forms: upload into this directory, create a subdirectory.
  {
    String enc = webFilesUrlEncode(dir);
    String forms;
    forms.reserve(420);
    forms += "<form method=\"POST\" action=\"/files/upload?dir=" + enc +
             "\" enctype=\"multipart/form-data\">"
             "<input type=\"file\" name=\"upload\"> "
             "<button type=\"submit\">Upload here</button></form><br>";
    forms += "<form method=\"POST\" action=\"/files/mkdir\">"
             "<input type=\"hidden\" name=\"dir\" value=\"" + webFilesHtmlEscape(dir) + "\">"
             "<input type=\"text\" name=\"name\" placeholder=\"new folder\"> "
             "<button type=\"submit\">Create folder</button></form>";
    forms += "<table>";
    screenshotServer.sendContent(forms);
  }

  // Entries, streamed one row at a time in FAT order (see header comment).
  File e;
  while ((e = d.openNextFile())) {
    String nm = e.name();
    int slash = nm.lastIndexOf('/');            // name() is basename on recent
    if (slash >= 0) nm = nm.substring(slash + 1); // cores, full path on older
    String child = (dir == "/") ? "/" + nm : dir + "/" + nm;
    String row;
    row.reserve(300);
    row += "<tr><td>";
    if (e.isDirectory()) {
      row += "<a href=\"/files?dir=" + webFilesUrlEncode(child) + "\">" +
             webFilesHtmlEscape(nm) + "/</a></td><td>&lt;DIR&gt;</td><td></td>";
    } else {
      // Name previews in the tab where the type allows it; the arrow forces a
      // save, so no right-click is needed for images.
      row += "<a href=\"/files/download?path=" + webFilesUrlEncode(child) + "\">" +
             webFilesHtmlEscape(nm) + "</a></td><td>" + String(e.size()) + "</td>"
             "<td><a class=\"dl\" title=\"Download\" href=\"/files/download?dl=1&amp;path=" +
             webFilesUrlEncode(child) + "\">&#8681;</a></td>";
    }
    row += "<td><form method=\"POST\" action=\"/files/delete\" "
           "onsubmit=\"return confirm('Delete this entry?')\">"
           "<input type=\"hidden\" name=\"path\" value=\"" +
           webFilesHtmlEscape(child) + "\">"
           "<button class=\"del\" type=\"submit\">x</button></form></td></tr>";
    e.close();
    screenshotServer.sendContent(row);
  }
  d.close();

  screenshotServer.sendContent_P(WEBFILES_PAGE_TAIL);
  screenshotServer.sendContent("");   // terminate chunked response
}

// -----------------------------------------------------------------------------
// GET /files/download — stream a file with a best-effort content type.
//
// Content-Disposition carries the real filename, which the browser otherwise
// has no way to learn: without it an .meta file saves as "download", because
// nothing in an octet-stream response names it. Viewable types are sent
// `inline` so images and text still open in the tab (and "Save image as…"
// then proposes the right name); everything else is `attachment`, so a click
// saves it straight away.
//
// The name is sent twice on purpose: the quoted ASCII form for older clients,
// and RFC 5987 `filename*` percent-encoded as UTF-8 for names with accents or
// other non-ASCII characters, which are common in ROM and artwork filenames.
// -----------------------------------------------------------------------------
static void handleWebFilesDownload() {
  if (!webConfigAuthOk()) return;
  String path = screenshotServer.arg("path");
  if (!webFilesPathOk(path)) {
    screenshotServer.send(400, "text/plain", "Invalid path");
    return;
  }
  File f = SD.open(path, FILE_READ);
  if (!f || f.isDirectory()) {
    if (f) f.close();
    screenshotServer.send(404, "text/plain", "Not found");
    return;
  }
  String p = path; p.toLowerCase();
  const char* type = "application/octet-stream";
  bool viewable = true;
  if      (p.endsWith(".jpg") || p.endsWith(".jpeg")) type = "image/jpeg";
  else if (p.endsWith(".png"))                        type = "image/png";
  else if (p.endsWith(".bmp"))                        type = "image/bmp";
  else if (p.endsWith(".ini") || p.endsWith(".bak") ||
           p.endsWith(".txt") || p.endsWith(".log") ||
           p.endsWith(".md"))                         type = "text/plain";
  else if (p.endsWith(".htm") || p.endsWith(".html")) type = "text/html";
  else                                                viewable = false;

  String name = path.substring(path.lastIndexOf('/') + 1);
  // ?dl=1 forces a download of a type that would otherwise open in the tab.
  // The listing offers both: the filename previews, the arrow saves.
  if (screenshotServer.hasArg("dl")) viewable = false;
  String ascii;                       // quoted-string form: no " or \ allowed
  ascii.reserve(name.length());
  for (size_t i = 0; i < name.length(); i++) {
    char c = name[i];
    if (c != '"' && c != '\\' && (uint8_t)c >= 0x20) ascii += c;
  }
  String disposition = viewable ? F("inline") : F("attachment");
  disposition += "; filename=\"" + ascii + "\"";
  disposition += "; filename*=UTF-8''" + webFilesUrlEncode(name);
  screenshotServer.sendHeader("Content-Disposition", disposition);

  screenshotServer.streamFile(f, type);
  f.close();
}

// -----------------------------------------------------------------------------
// POST /files/delete — file or EMPTY directory, then redirect to the parent
// -----------------------------------------------------------------------------
static void handleWebFilesDelete() {
  if (!webConfigAuthOk()) return;
  String path = screenshotServer.arg("path");
  if (!webFilesPathOk(path) || path == "/") {
    screenshotServer.send(400, "text/plain", "Invalid path");
    return;
  }
  bool isDir = false;
  {
    File f = SD.open(path);
    if (!f) {
      screenshotServer.send(404, "text/plain", "Not found");
      return;
    }
    isDir = f.isDirectory();
    f.close();
  }
  bool ok = isDir ? SD.rmdir(path) : SD.remove(path);
  if (!ok) {
    screenshotServer.send(500, "text/plain",
        isDir ? "Delete failed - directory not empty?" : "Delete failed");
    return;
  }
  Serial.printf("[WEBFILES] Deleted %s\n", path.c_str());
  webFilesRedirectToDir(webFilesParentDir(path));
}

// -----------------------------------------------------------------------------
// POST /files/mkdir — create a subdirectory, then redirect to the parent
// -----------------------------------------------------------------------------
static void handleWebFilesMkdir() {
  if (!webConfigAuthOk()) return;
  String dir  = screenshotServer.arg("dir");
  String name = screenshotServer.arg("name");
  name.trim();
  if (!webFilesPathOk(dir) || name.length() == 0 ||
      name.indexOf('/') >= 0 || name.indexOf('\\') >= 0 ||
      name.indexOf("..") >= 0) {
    screenshotServer.send(400, "text/plain", "Invalid directory name");
    return;
  }
  String path = (dir == "/") ? "/" + name : dir + "/" + name;
  if (!webFilesPathOk(path) || !SD.mkdir(path)) {
    screenshotServer.send(500, "text/plain", "mkdir failed");
    return;
  }
  Serial.printf("[WEBFILES] Created %s\n", path.c_str());
  webFilesRedirectToDir(dir);
}

// -----------------------------------------------------------------------------
// POST /files/upload — multipart upload, streamed chunk-by-chunk to SD.
// The target directory travels in the form action's query string (?dir=…),
// which WebServer parses before the body, so it is available at
// UPLOAD_FILE_START. Auth is verified at START too: with a password set, an
// unauthenticated POST must not leave a file on the card even though the
// 401 itself is only issued by the completion handler.
// -----------------------------------------------------------------------------
static File   webFilesUploadFile;
static String webFilesUploadPath;
static String webFilesUploadDir   = "/";
static String webFilesUploadError;

static void handleWebFilesUploadChunk() {
  HTTPUpload& up = screenshotServer.upload();

  switch (up.status) {
    case UPLOAD_FILE_START: {
      webFilesUploadError = "";
      webFilesUploadPath  = "";
      webFilesUploadDir   = screenshotServer.hasArg("dir")
                              ? screenshotServer.arg("dir") : "/";
      if (appConfig.webPassword.length() > 0 &&
          !screenshotServer.authenticate("admin", appConfig.webPassword.c_str())) {
        webFilesUploadError = F("Authentication required");
        return;
      }
      String name = up.filename;
      int slash = name.lastIndexOf('/');
      if (slash >= 0) name = name.substring(slash + 1);
      slash = name.lastIndexOf('\\');
      if (slash >= 0) name = name.substring(slash + 1);
      name.trim();
      if (!webFilesPathOk(webFilesUploadDir) || name.length() == 0 ||
          name.indexOf("..") >= 0) {
        webFilesUploadError = F("Invalid target path");
        return;
      }
      String path = (webFilesUploadDir == "/") ? "/" + name
                                               : webFilesUploadDir + "/" + name;
      if (!webFilesPathOk(path)) {
        webFilesUploadError = F("Invalid target path");
        return;
      }
      SD.remove(path);   // overwrite semantics; also clears a stale partial
      webFilesUploadFile = SD.open(path, FILE_WRITE);
      if (!webFilesUploadFile) {
        webFilesUploadError = F("Cannot open target file for writing");
        return;
      }
      webFilesUploadPath = path;
      Serial.printf("[WEBFILES] Upload start: %s\n", path.c_str());
      break;
    }

    case UPLOAD_FILE_WRITE:
      if (webFilesUploadFile) {
        if (webFilesUploadFile.write(up.buf, up.currentSize) != up.currentSize) {
          webFilesUploadFile.close();
          SD.remove(webFilesUploadPath);
          webFilesUploadPath  = "";
          webFilesUploadError = F("Short write to SD card");
        }
      }
      break;

    case UPLOAD_FILE_END:
      if (webFilesUploadFile) webFilesUploadFile.close();
      if (webFilesUploadPath.length() > 0 &&
          webFilesIsJpegName(webFilesUploadPath) &&
          webFilesJpegIsProgressive(webFilesUploadPath)) {
        SD.remove(webFilesUploadPath);
        webFilesUploadPath  = "";
        webFilesUploadError = F("Progressive JPEG rejected - the firmware's decoder "
                                "(JPEGDEC) only handles baseline JPEG. Re-export the "
                                "image as baseline and upload again.");
      } else if (webFilesUploadPath.length() > 0) {
        Serial.printf("[WEBFILES] Upload done: %s (%u bytes)\n",
                      webFilesUploadPath.c_str(), (unsigned)up.totalSize);
      }
      break;

    case UPLOAD_FILE_ABORTED:
      if (webFilesUploadFile) webFilesUploadFile.close();
      if (webFilesUploadPath.length() > 0) SD.remove(webFilesUploadPath);
      webFilesUploadPath  = "";
      webFilesUploadError = F("Upload aborted");
      break;
  }
}

static void handleWebFilesUploadDone() {
  if (!webConfigAuthOk()) return;
  if (webFilesUploadError.length() > 0) {
    screenshotServer.send(500, "text/plain", webFilesUploadError);
    return;
  }
  webFilesRedirectToDir(webFilesUploadDir);
}

// -----------------------------------------------------------------------------
// Public entry point — call next to registerWebConfigRoutes() (attach boards)
// or right after startWebConfigServerStandalone() (CYD35C/R). Registering a
// route after begin() is fine with WebServer.
// -----------------------------------------------------------------------------
inline void registerWebFilesRoutes() {
  if (!appConfig.webConfig) {
    Serial.println("[WEBFILES] Disabled via [ui] web_config=false");
    return;
  }
  screenshotServer.on("/files",          HTTP_GET,  handleWebFilesList);
  screenshotServer.on("/files/download", HTTP_GET,  handleWebFilesDownload);
  screenshotServer.on("/files/delete",   HTTP_POST, handleWebFilesDelete);
  screenshotServer.on("/files/mkdir",    HTTP_POST, handleWebFilesMkdir);
  screenshotServer.on("/files/upload",   HTTP_POST,
                      handleWebFilesUploadDone, handleWebFilesUploadChunk);
  Serial.printf("[WEBFILES] SD browser at http://%s:8080/files\n",
                WiFi.localIP().toString().c_str());
}
