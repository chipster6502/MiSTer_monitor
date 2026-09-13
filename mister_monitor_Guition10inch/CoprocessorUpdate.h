// MiSTer Monitor
// Copyright (C) 2025-2026 chipster6502
// SPDX-License-Identifier: GPL-3.0-or-later

// =============================================================================
// CoprocessorUpdate.h -- keeps the ESP32-C6 radio firmware in step with the host
// =============================================================================
// The ESP32-P4 has no radio of its own. WiFi runs on an ESP32-C6 on the same
// board, reached over SDIO, and that C6 runs its own ESP-Hosted firmware. The
// host side of that protocol ships inside the Arduino core, so the two have to
// match: Guition ships these boards with an old slave build (v2.3.2 as of
// writing) which answers scans perfectly but can never complete an association.
// The result is a display that lists your network at full signal strength and
// then sits at WL_DISCONNECTED forever -- see issue #<n>.
//
// The core can update the C6 itself, and its updateEspHostedSlave() does
// exactly that: fetch the matching image over HTTPS, then push it to the C6 in
// small chunks. Only the fetch is unusable here, because it needs the WiFi that
// is broken. So this reads the same image from the SD card instead and drives
// the same four core primitives:
//
//     hostedBeginUpdate() -> hostedWriteUpdate() xN -> hostedEndUpdate()
//                         -> hostedActivateUpdate()
//
// Nothing here touches the network stack. Whether the C6 accepts an update with
// no association in place is the one thing that could not be verified before
// shipping this -- if it refuses, hostedBeginUpdate() fails and the user is told
// what to do, which is no worse than the situation without this file.
//
// Deliberately not asking for confirmation. A board that needs this update is
// very often a board whose screen cannot be read: the same production batches
// that carry old C6 firmware also carry the panel revision that renders as
// banding until panel_rev=v2 is set. Asking permission through the one channel
// that may be broken helps nobody. The write is also safer than it sounds: the
// image goes to the C6's spare OTA slot and only becomes live at
// hostedActivateUpdate(), so losing power mid-write leaves the old firmware in
// place and the check simply runs again next boot.
//
// Getting the image: the URL is version-specific, and the core knows which one
// belongs to it. hostedGetUpdateURL() returns it, e.g.
//     https://espressif.github.io/arduino-esp32/hosted/esp32c6-v2.12.11.bin
// That file goes on the card as /c6_firmware.bin, and ships in
// SD_card_content/Guition/. It is Espressif's build, Apache-2.0.
//
// Call site: after WiFi.mode(WIFI_STA), before WiFi.begin(). The SDIO link is
// only alive once the radio has been put into station mode -- ask any earlier
// and hostedGetSlaveVersion() reports 0.0.0 whatever the C6 actually runs,
// which would look like an ancient build on a perfectly healthy board.
// =============================================================================

#pragma once

#include <Arduino.h>
#include <FS.h>
#include "esp32-hal-hosted.h"

// Espressif's own OTA loop uses 2 KB chunks; no reason to differ.
static const size_t   C6_UPDATE_CHUNK      = 2048;
static const char    *C6_FIRMWARE_PATH     = "/c6_firmware.bin";
// A truncated download would otherwise be pushed to the C6 as if it were valid.
// The real images are ~1.3 MB; anything far below that is not one.
static const uint32_t C6_FIRMWARE_MIN_SIZE = 512UL * 1024UL;

struct C6UpdateStatus {
  bool     needed   = false;   // slave older than host
  bool     attempted= false;
  bool     ok       = false;
  uint32_t hostVer[3]  = {0, 0, 0};
  uint32_t slaveVer[3] = {0, 0, 0};
  String   message;            // one line, fit to show on screen
};

// Compares as a version triple rather than a hardcoded threshold: the host side
// lives in the core, so "old enough to matter" is simply "older than the host",
// which is the same test updateEspHostedSlave() applies.
static bool c6SlaveOlderThanHost(const uint32_t s[3], const uint32_t h[3]) {
  for (int i = 0; i < 3; i++) {
    if (s[i] < h[i]) return true;
    if (s[i] > h[i]) return false;
  }
  return false;
}

// Runs the update from the image on the card. `fs` is the mounted SD.
// `onProgress` is optional and receives 0..100 so the caller can draw a bar
// without this file knowing anything about the display.
inline C6UpdateStatus c6UpdateIfNeeded(fs::FS &fs,
                                       void (*onProgress)(int) = nullptr) {
  C6UpdateStatus st;

  hostedGetHostVersion (&st.hostVer[0],  &st.hostVer[1],  &st.hostVer[2]);
  hostedGetSlaveVersion(&st.slaveVer[0], &st.slaveVer[1], &st.slaveVer[2]);

  Serial.printf("[C6] host %u.%u.%u | slave %u.%u.%u\n",
                st.hostVer[0], st.hostVer[1], st.hostVer[2],
                st.slaveVer[0], st.slaveVer[1], st.slaveVer[2]);

  // 0.0.0 means the SDIO link has not answered, not that the C6 is ancient.
  // Called too early this is the normal reading, so treat it as "unknown" and
  // leave the board alone rather than reflashing a healthy coprocessor.
  if (st.slaveVer[0] == 0 && st.slaveVer[1] == 0 && st.slaveVer[2] == 0) {
    st.message = F("Coprocessor version unavailable - skipping update check");
    Serial.printf("[C6] %s\n", st.message.c_str());
    return st;
  }

  if (!c6SlaveOlderThanHost(st.slaveVer, st.hostVer)) {
    Serial.println("[C6] Coprocessor firmware is current");
    return st;
  }

  st.needed = true;

  File f = fs.open(C6_FIRMWARE_PATH, FILE_READ);
  if (!f || f.isDirectory()) {
    if (f) f.close();
    st.message = String(F("WiFi needs a coprocessor update; ")) +
                 C6_FIRMWARE_PATH + F(" is missing from the card");
    Serial.printf("[C6] %s\n", st.message.c_str());
    return st;
  }

  const uint32_t total = f.size();
  if (total < C6_FIRMWARE_MIN_SIZE) {
    f.close();
    st.message = F("Coprocessor image on the card looks truncated - not used");
    Serial.printf("[C6] %s (%u bytes)\n", st.message.c_str(), (unsigned)total);
    return st;
  }

  st.attempted = true;
  Serial.printf("[C6] Updating from %s (%u bytes)\n",
                C6_FIRMWARE_PATH, (unsigned)total);

  if (!hostedBeginUpdate()) {
    f.close();
    // The one path that could not be tested before release: if the C6 turns out
    // to require an association before accepting an update, this is where it
    // shows up, and the message has to be enough on its own.
    st.message = F("Coprocessor rejected the update - try again with the "
                   "display on any working network");
    Serial.printf("[C6] %s\n", st.message.c_str());
    return st;
  }

  uint8_t  buf[C6_UPDATE_CHUNK];
  uint32_t written  = 0;
  int      lastPct  = -1;
  bool     failed   = false;

  while (written < total) {
    size_t n = f.read(buf, sizeof(buf));
    if (n == 0) {
      st.message = F("Read error on the coprocessor image");
      failed = true;
      break;
    }
    if (!hostedWriteUpdate(buf, (uint32_t)n)) {
      st.message = F("Coprocessor write failed - old firmware left in place");
      failed = true;
      break;
    }
    written += n;

    int pct = (int)((written * 100ULL) / total);
    if (pct != lastPct) {
      lastPct = pct;
      if (onProgress) onProgress(pct);
      if (pct % 10 == 0) Serial.printf("[C6] %d%%\n", pct);
    }
  }
  f.close();

  if (failed) {
    Serial.printf("[C6] %s\n", st.message.c_str());
    return st;   // nothing activated, so the C6 still runs its old firmware
  }

  if (!hostedEndUpdate()) {
    st.message = F("Coprocessor update did not verify - old firmware kept");
    Serial.printf("[C6] %s\n", st.message.c_str());
    return st;
  }

  if (!hostedActivateUpdate()) {
    st.message = F("Coprocessor update could not be activated");
    Serial.printf("[C6] %s\n", st.message.c_str());
    return st;
  }

  st.ok = true;
  st.message = F("WiFi coprocessor updated - restarting");
  Serial.printf("[C6] %s\n", st.message.c_str());
  return st;
}
