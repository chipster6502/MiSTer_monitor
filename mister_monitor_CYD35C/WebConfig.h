// MiSTer Monitor
// Copyright (C) 2025-2026 chipster6502
// SPDX-License-Identifier: GPL-3.0-or-later

// =============================================================================
// WebConfig.h  —  Web-based config.ini editor and reboot endpoint
// =============================================================================
// Serves three routes on the existing device web server (port 8080):
//
//   GET  /config   — /config.ini from the microSD in a plain <textarea>
//   POST /config   — writes the textarea content back to the card (safe write)
//   POST /reboot   — replies first, then ESP.restart()
//
// Integration (per board, in setup(), after WiFi is connected):
//   * Boards that start the screenshot server (CYD28 x2, Tab5, Guition10inch):
//     call registerWebConfigRoutes() right before setupScreenshotServer().
//     The routes attach to the same WebServer instance.
//   * Boards whose panel has no framebuffer readback (CYD35C, CYD35R): call
//     startWebConfigServerStandalone() instead — it registers the routes,
//     adds a landing page at "/", and starts the server.
//
// Safe write: the new content goes to /config.ini.new first; the previous
// file is kept as /config.ini.bak. FatFs f_rename() rejects an existing
// destination, so every rename is preceded by a remove of its target — the
// same idiom the artwork downloader uses. If power is lost mid-write, the
// old /config.ini survives untouched; if the final rename fails, the new
// content is still on the card as /config.ini.new for manual recovery.
//
// The editor is deliberately a raw text editor, not a form: config.ini
// documents every key in inline comments, and the boot parser already
// tolerates CRLF, [sections] and unknown keys — so the web path adds no
// failure mode that editing the file on a PC doesn't already have.
//
// This file is byte-identical across all board folders. It only touches
// SD (an alias of SD_MMC on the ESP32-P4 boards), the sketch's WebServer
// instance and appConfig, so no per-board variant is needed.
//
// config.ini keys (parsed in AppConfig.h):
//   [ui] web_config   = true / false — false disables all three routes
//   [ui] web_password =              — non-empty enables HTTP Basic Auth
//                                      (user "admin", this value as password)
// =============================================================================

#pragma once
#include <Arduino.h>
#include <FS.h>   // File, FILE_READ/FILE_WRITE - NOT <SD.h>: the sketch
                  // already provides the SD object, and its type differs
                  // per board (fs::SDFS on the SPI boards, an SD_MMC
                  // alias on the Guition). Including <SD.h> here would
                  // redeclare SD and break those boards.
#include <WiFi.h>
#include <WebServer.h>
#include "AppConfig.h"

extern WebServer screenshotServer;   // defined in the sketch (port 8080)
extern AppConfig appConfig;          // defined in the sketch, loaded at boot

// Measured config.ini sizes as shipped: ~9.9 KB (Tab5/CYD35 layouts) and
// ~10.5 KB (CYD28 layouts). 32 KB leaves 3x headroom for user comments.
// Note: WebServer has already buffered the POST body in RAM by the time the
// handler runs, so this cap protects the SD write and produces a clear error
// — it cannot prevent the initial allocation.
static const size_t WEBCONFIG_MAX_INI_BYTES = 32768;

// -----------------------------------------------------------------------------
// HTML — kept in flash, styled to match the existing screenshot page
// (black background, cyan monospace) so both feel like the same device.
// -----------------------------------------------------------------------------
static const char WEBCONFIG_STYLE[] PROGMEM = R"rawliteral(<style>
body{background:#000;color:#0FF;font-family:monospace;margin:0;padding:16px}
h1{font-size:18px;margin:0 0 12px}
textarea{width:100%;height:70vh;background:#111;color:#0F0;border:1px solid #0FF;
font-family:monospace;font-size:13px;box-sizing:border-box}
button{background:#0FF;color:#000;border:0;padding:8px 20px;font-family:monospace;
font-weight:bold;margin:10px 8px 0 0;cursor:pointer}
.danger{background:#F55}
form{display:inline}
a{color:#0FF}
p.note{color:#888;font-size:12px}
</style>)rawliteral";

static const char WEBCONFIG_EDITOR_HEAD[] PROGMEM = R"rawliteral(<!DOCTYPE html><html><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>MiSTer Monitor - config.ini</title>)rawliteral";

static const char WEBCONFIG_EDITOR_BODY_OPEN[] PROGMEM = R"rawliteral(</head><body>
<h1>MiSTer Monitor &mdash; config.ini</h1>
<form method="POST" action="/config" accept-charset="utf-8">
<textarea name="ini" spellcheck="false" wrap="off">)rawliteral";

static const char WEBCONFIG_EDITOR_TAIL[] PROGMEM = R"rawliteral(</textarea>
<br><button type="submit">Save</button></form>
<form method="POST" action="/reboot" onsubmit="return confirm('Reboot the display?')">
<button class="danger" type="submit">Reboot</button></form>
<p><a href="/files">Browse the SD card</a></p>
<p class="note">Changes take effect after a reboot. The previous file is kept on the
card as config.ini.bak.</p>
</body></html>)rawliteral";

static const char WEBCONFIG_SAVED_PAGE[] PROGMEM = R"rawliteral(<!DOCTYPE html><html><head>
<meta charset="utf-8"><title>MiSTer Monitor - saved</title></head><body
style="background:#000;color:#0FF;font-family:monospace;padding:16px">
<h1>config.ini saved</h1>
<p>The previous file was kept as config.ini.bak. Reboot to apply the new settings.</p>
<form method="POST" action="/reboot"><button
style="background:#F55;color:#000;border:0;padding:8px 20px;font-family:monospace;font-weight:bold;cursor:pointer"
type="submit">Reboot now</button></form>
<p><a style="color:#0FF" href="/config">Back to the editor</a></p>
</body></html>)rawliteral";

static const char WEBCONFIG_REBOOT_PAGE[] PROGMEM = R"rawliteral(<!DOCTYPE html><html><head>
<meta charset="utf-8"><meta http-equiv="refresh" content="15;url=/config">
<title>MiSTer Monitor - rebooting</title></head><body
style="background:#000;color:#0FF;font-family:monospace;padding:16px">
<h1>Rebooting&hellip;</h1>
<p>The display is restarting. This page will reload the editor in 15 seconds.</p>
</body></html>)rawliteral";

static const char WEBCONFIG_LANDING_PAGE[] PROGMEM = R"rawliteral(<!DOCTYPE html><html><head>
<meta charset="utf-8"><title>MiSTer Monitor</title></head><body
style="background:#000;color:#0FF;font-family:monospace;padding:16px">
<h1>MiSTer Monitor</h1>
<p><a style="color:#0FF" href="/config">Edit config.ini</a></p>
<p><a style="color:#0FF" href="/files">Browse the SD card</a></p>
<p style="color:#888;font-size:12px">Screenshot capture is not available on this board
(the panel does not support framebuffer readback).</p>
</body></html>)rawliteral";

// -----------------------------------------------------------------------------
// Auth gate — no-op when [ui] web_password is empty (default).
// -----------------------------------------------------------------------------
static bool webConfigAuthOk() {
  if (appConfig.webPassword.length() == 0) return true;
  if (screenshotServer.authenticate("admin", appConfig.webPassword.c_str())) return true;
  screenshotServer.requestAuthentication();   // 401 + Basic challenge
  return false;
}

// -----------------------------------------------------------------------------
// Stream /config.ini into the <textarea>, HTML-escaping &, <, > so that no
// character in a password or comment can break out of the element. Streamed
// in small chunks instead of one big String: the heap on the CYD boards can
// be tight while an artwork operation is in flight, and handleClient() is
// pumped from inside those operations (pumpedDelay).
// -----------------------------------------------------------------------------
static void webConfigSendEscapedFile(File& f) {
  char raw[256];
  String out;
  out.reserve(sizeof(raw) * 2);
  while (f.available()) {
    size_t n = f.read((uint8_t*)raw, sizeof(raw));
    if (n == 0) break;
    out = "";
    for (size_t i = 0; i < n; i++) {
      char c = raw[i];
      if      (c == '&') out += F("&amp;");
      else if (c == '<') out += F("&lt;");
      else if (c == '>') out += F("&gt;");
      else               out += c;
    }
    screenshotServer.sendContent(out);
  }
}

// -----------------------------------------------------------------------------
// GET /config — editor page with the current file. Chunked response: header,
// escaped file content, tail, then an empty chunk to terminate.
// -----------------------------------------------------------------------------
static void handleWebConfigGet() {
  if (!webConfigAuthOk()) return;

  screenshotServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
  screenshotServer.send(200, "text/html", "");
  screenshotServer.sendContent_P(WEBCONFIG_EDITOR_HEAD);
  screenshotServer.sendContent_P(WEBCONFIG_STYLE);
  screenshotServer.sendContent_P(WEBCONFIG_EDITOR_BODY_OPEN);

  File f = SD.open("/config.ini", FILE_READ);
  if (f) {
    webConfigSendEscapedFile(f);
    f.close();
  } else {
    screenshotServer.sendContent(
        "; /config.ini not found on the SD card - saving will create it\n");
  }

  screenshotServer.sendContent_P(WEBCONFIG_EDITOR_TAIL);
  screenshotServer.sendContent("");   // terminate chunked response
}

// -----------------------------------------------------------------------------
// POST /config — safe write:
//   1) content -> /config.ini.new (verified length)
//   2) /config.ini -> /config.ini.bak   (remove .bak first: f_rename refuses
//   3) /config.ini.new -> /config.ini    existing destinations)
// -----------------------------------------------------------------------------
static void handleWebConfigSave() {
  if (!webConfigAuthOk()) return;

  if (!screenshotServer.hasArg("ini")) {
    screenshotServer.send(400, "text/plain", "Missing 'ini' form field");
    return;
  }
  const String& body = screenshotServer.arg("ini");
  if (body.length() > WEBCONFIG_MAX_INI_BYTES) {
    screenshotServer.send(413, "text/plain", "config.ini too large (max 32 KB)");
    return;
  }

  // 1) Scratch file first. FILE_WRITE truncates on the ESP32 FS layer, but
  //    remove first anyway so a stale .new from an earlier failure can't mix in.
  SD.remove("/config.ini.new");
  File f = SD.open("/config.ini.new", FILE_WRITE);
  if (!f) {
    screenshotServer.send(500, "text/plain", "Cannot open /config.ini.new for writing");
    return;
  }
  size_t written = f.print(body);
  f.close();
  if (written != body.length()) {
    SD.remove("/config.ini.new");
    screenshotServer.send(500, "text/plain",
        "Short write to SD card - config.ini left untouched");
    return;
  }

  // 2) Rotate current -> .bak. If the rename is refused with the file still
  //    present (unusual), drop the backup rather than fail the save.
  SD.remove("/config.ini.bak");
  if (SD.exists("/config.ini") && !SD.rename("/config.ini", "/config.ini.bak")) {
    Serial.println("[WEBCONFIG] rename to .bak refused - saving without backup");
    SD.remove("/config.ini");
  }

  // 3) Promote the scratch file.
  if (!SD.rename("/config.ini.new", "/config.ini")) {
    screenshotServer.send(500, "text/plain",
        "Rename failed - new content is on the card as /config.ini.new");
    return;
  }

  Serial.printf("[WEBCONFIG] Saved /config.ini (%u bytes)\n", (unsigned)written);
  screenshotServer.send_P(200, "text/html", WEBCONFIG_SAVED_PAGE);
}

// -----------------------------------------------------------------------------
// POST /reboot — reply first, give the TCP stack a moment to flush, restart.
// There is no separate "client process" on the ESP: WiFi, display flip, media
// orders etc. are copied out of appConfig once in setup(), so a full restart
// is the honest way to apply a new config.
// -----------------------------------------------------------------------------
static void handleWebConfigReboot() {
  if (!webConfigAuthOk()) return;
  Serial.println("[WEBCONFIG] Reboot requested via /reboot");
  screenshotServer.send_P(200, "text/html", WEBCONFIG_REBOOT_PAGE);
  delay(250);
  ESP.restart();
}

// -----------------------------------------------------------------------------
// Landing page at "/" — only registered in standalone mode (boards without
// the screenshot page), so the root URL is not a 404 there.
// -----------------------------------------------------------------------------
static void handleWebConfigLanding() {
  screenshotServer.send_P(200, "text/html", WEBCONFIG_LANDING_PAGE);
}

// -----------------------------------------------------------------------------
// Public entry points
// -----------------------------------------------------------------------------

// Boards that already run the screenshot server: call this right before
// setupScreenshotServer(). Registration order vs. begin() does not matter to
// WebServer, but registering first keeps setup() reading top-to-bottom.
inline void registerWebConfigRoutes() {
  if (!appConfig.webConfig) {
    Serial.println("[WEBCONFIG] Disabled via [ui] web_config=false");
    return;
  }
  screenshotServer.on("/config", HTTP_GET,  handleWebConfigGet);
  screenshotServer.on("/config", HTTP_POST, handleWebConfigSave);
  screenshotServer.on("/reboot", HTTP_POST, handleWebConfigReboot);
  Serial.printf("[WEBCONFIG] Editor at http://%s:8080/config\n",
                WiFi.localIP().toString().c_str());
}

// Boards without framebuffer readback (no screenshot server): registers the
// config routes plus a landing page at "/", and starts the server itself.
inline void startWebConfigServerStandalone() {
  if (!appConfig.webConfig) {
    Serial.println("[WEBCONFIG] Disabled via [ui] web_config=false");
    return;
  }
  registerWebConfigRoutes();
  screenshotServer.on("/", handleWebConfigLanding);
  screenshotServer.begin();
  Serial.printf("[WEBCONFIG] Web server running at http://%s:8080/\n",
                WiFi.localIP().toString().c_str());
}
