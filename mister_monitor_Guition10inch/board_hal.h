// MiSTer Monitor
// Copyright (C) 2025-2026 chipster6502
// SPDX-License-Identifier: GPL-3.0-or-later

// ============================================================================
//  Compatibility layer for the Guition JC8012P4A1C (10.1" ESP32-P4)
//
//  Provides the same `display` + `Board` facade as the CYD ports, so the
//  sketch's drawing and input logic stays hardware-agnostic.
//
//  Hardware:
//    - Panel:   800x1280 MIPI-DSI, JD9365 controller, software-rotated to
//               landscape. The sketch draws on a 1280x720 window centred on
//               the panel (see Panel_JD9365.hpp).
//    - Touch:   GSL3680 capacitive over I2C, via the vendor esp_lcd driver.
//    - Storage: microSD on the SDMMC peripheral, slot 0, IO bank powered by
//               the on-chip LDO channel 4. The core's ESP32P4 Dev Module
//               variant already selects that slot and LDO for SD_MMC. It also
//               pulses its reference board's SD power switch on GPIO45, which
//               here is only an expansion-header pin.
//    - WiFi:    ESP32-C6 coprocessor over SDIO (ESP-Hosted), handled by the
//               core. No pin setup needed; connection pattern in the .ino.
//
//  Confirmed pinout (vendor schematic):
//    LCD:    RST=27  BL=23 (PWM, active high)     D-PHY power: LDO channel 3
//    Touch:  SDA=7   SCL=8   RST=22  INT=21
//    SD:     CLK=43  CMD=44  D0..D3=39..42         IO power:    LDO channel 4
//    C6:     SDIO CLK=18 CMD=19 D0..D3=14..17 RST=54 (esp_hosted defaults)
//    Other:  WS2812 LED data=26, audio codec on I2S (both unused)
// ============================================================================

#pragma once

#include <M5GFX.h>
#include <SD_MMC.h>
#include "Panel_JD9365.hpp"
#include "src/touch/gsl3680_touch.h"

// ---------- Touch wiring ---------------------------------------------------
#define JC8012_TP_SDA  7
#define JC8012_TP_SCL  8
#define JC8012_TP_RST  22
#define JC8012_TP_INT  21

// The sketch uses the Arduino FS API through `SD`; on this board that is the
// SDMMC card. Same call sites as the SPI boards, different transport.
static fs::SDMMCFS& SD = SD_MMC;

extern m5gfx_user::LGFX_JC8012P4A1C display;

// ---------- Touch state structs (identical contract to the CYD ports) ------

struct TouchDetail {
  bool _pressed = false;
  int  x = 0;
  int  y = 0;
  bool wasPressed() const { return _pressed; }
};

struct BoardTouch {
  // Latest polled state, refreshed by Board.update().
  TouchDetail _current;
  TouchDetail getDetail() { return _current; }
};

struct BoardSpeaker {
  // No-op: the board has an audio codec on I2S, but this port does not drive it.
  void begin() {}
  void setVolume(int) {}
  void tone(int /*freq*/, int /*duration_ms*/) {}
};

struct BoardConfig {
  bool clear_display = true;
  bool output_power  = true;
  bool internal_imu  = false;
  bool external_imu  = false;
};

struct BoardClass {
  m5gfx_user::LGFX_JC8012P4A1C& Display = display;
  BoardTouch    Touch;
  BoardSpeaker  Speaker;

  BoardConfig config() { return BoardConfig(); }

  void begin(const BoardConfig& cfg) {
    display.init();                  // vendor DSI bring-up, rotation 1
    display.setColorDepth(16);
    if (cfg.clear_display) {
      display.fillScreen(TFT_BLACK);
    }
    display.setBrightness(204);      // 80%

    // Touch is optional: the display keeps working if the controller fails.
    _touchReady = _touch.begin();
    if (_touchReady) {
      // Portrait controller onto the landscape canvas; mapping confirmed on hardware.
      _touch.setRotationLandscape(false, false);
    }
  }

  bool touchReady() const { return _touchReady; }

  // Mount the card. Slot, pins and LDO come from the board variant.
  bool beginSD() {
    return SD_MMC.begin("/sdcard", false);
  }

  void update() {
    // wasPressed() == true for exactly one update cycle on the press edge.
    Touch._current._pressed = false;
    if (!_touchReady) return;

    // The controller is polled over I2C; a short cadence is plenty for taps.
    const unsigned long now = millis();
    if (now - _lastPoll < TOUCH_POLL_MS) return;
    _lastPoll = now;

    uint16_t rx = 0, ry = 0;
    const bool nowPressed = _touch.getTouch(&rx, &ry);
    if (nowPressed) {
      int tx = (int)rx;
      int ty = (int)ry - JD9365_UI_INSET;    // panel space -> UI window space

      // A 180-degree flip of the display mirrors both axes of the touch too.
      if ((display.getRotation() & 3) == 3) {
        tx = display.width()  - 1 - tx;
        ty = display.height() - 1 - ty;
      }
      // Touches on the hidden bands land on the nearest UI edge.
      if (tx < 0) tx = 0;
      if (ty < 0) ty = 0;
      if (tx > display.width()  - 1) tx = display.width()  - 1;
      if (ty > display.height() - 1) ty = display.height() - 1;

      Touch._current.x = tx;
      Touch._current.y = ty;
    }
    Touch._current._pressed = (nowPressed && !_lastTouchState);
    _lastTouchState = nowPressed;
  }

private:
  static constexpr unsigned long TOUCH_POLL_MS = 10;
  gsl3680_touch _touch{JC8012_TP_SDA, JC8012_TP_SCL, JC8012_TP_RST, JC8012_TP_INT};
  bool          _touchReady     = false;
  bool          _lastTouchState = false;
  unsigned long _lastPoll       = 0;
};

extern BoardClass Board;
