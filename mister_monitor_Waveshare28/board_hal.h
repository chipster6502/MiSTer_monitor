// MiSTer Monitor
// Copyright (C) 2025-2026 chipster6502
// SPDX-License-Identifier: GPL-3.0-or-later

// ============================================================================
//  Compatibility layer for the Waveshare ESP32-S3-Touch-LCD-2.8 (V2)
//
//  Drop-in replacement for the CYD28R_ST7789 board_hal.h: the Board facade
//  (Display / Touch / Speaker) is kept identical, so the .ino only differs in
//  the SD pins, the standby backlight pin and the brownout guard.
//
//  Display calls are routed to a LovyanGFX instance configured for the
//  board's ST7789 panel (240x320 native, used in landscape); Touch reads the
//  CST3530 capacitive controller over I2C; Speaker is a no-op (the board has
//  a PCM5101 I2S DAC + speaker, but the 2.8" sketches do not use sound).
//
//  Pinout (from Waveshare's V2 demo, ESP32-S3-Touch-LCD-2.8-V2-Demo.zip,
//  LVGL_Arduino: Display_ST7789.h / Touch_CST3530.h / SD_Card.h):
//    TFT (SPI2/FSPI):  SCK=40 MOSI=45 MISO=nc DC=41 CS=42 RST=39 BL=5
//    SD  (SPI3/HSPI):  SCK=14 MOSI(CMD)=17 MISO(D0)=16 CS(D3)=21  (in .ino)
//    Touch CST3530 (I2C on Wire1, addr 0x58): SDA=1 SCL=3 INT=4 RST=2
//    Power latch: GPIO7 must be driven HIGH, or the board switches itself
//                 off when it runs from the LiPo.
//    Sensor I2C (PCF85063 RTC 0x51, QMI8658 IMU 0x6B): SDA=11 SCL=10, unused.
//
//  Identify the V2: an I2C scan on SDA=1/SCL=3 finds the CST3530 at 0x58.
//  The V1 board has a different touch controller and is not covered here.
//  The non-touch "ESP32-S3-LCD-2.8" and the "-2.8B" (ST7701, 480x640 RGB)
//  are different boards as well.
// ============================================================================

#pragma once

#include <LovyanGFX.hpp>
#include <Wire.h>

// ---------- LovyanGFX panel class ------------------------------------------

class LGFX_WS28 : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789  _panel_instance;
  lgfx::Bus_SPI       _bus_instance;
  lgfx::Light_PWM     _light_instance;

public:
  LGFX_WS28(void) {
    {
      auto cfg = _bus_instance.config();
      cfg.spi_host    = SPI2_HOST;
      cfg.spi_mode    = 0;
      cfg.freq_write  = 80000000;         // Waveshare's demo runs at 80 MHz
      cfg.freq_read   = 16000000;
      cfg.spi_3wire   = false;
      cfg.use_lock    = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = 40;
      cfg.pin_mosi = 45;
      cfg.pin_miso = -1;
      cfg.pin_dc   = 41;
      _bus_instance.config(cfg);
      _panel_instance.setBus(&_bus_instance);
    }
    {
      auto cfg = _panel_instance.config();
      cfg.pin_cs           = 42;
      cfg.pin_rst          = 39;
      cfg.pin_busy         = -1;
      cfg.panel_width      = 240;
      cfg.panel_height     = 320;
      cfg.offset_x         = 0;
      cfg.offset_y         = 0;
      cfg.offset_rotation  = 0;
      cfg.dummy_read_pixel = 16;
      cfg.dummy_read_bits  = 1;
      cfg.readable         = false;       // no MISO wired: /screenshot.bmp is black
      cfg.invert           = true;        // the demo sends INVON (0x21)
      cfg.rgb_order        = false;
      cfg.dlen_16bit       = false;
      cfg.bus_shared       = false;       // the SD card has its own SPI bus
      _panel_instance.config(cfg);
    }
    {
      auto cfg = _light_instance.config();
      cfg.pin_bl      = 5;
      cfg.invert      = false;
      cfg.freq        = 44100;
      cfg.pwm_channel = 7;
      _light_instance.config(cfg);
      _panel_instance.setLight(&_light_instance);
    }
    setPanel(&_panel_instance);
  }
};

// The .ino never names the type, only `display`.
using LGFX_CYD = LGFX_WS28;
extern LGFX_CYD display;

// ---------- CST3530 capacitive touch (I2C, Wire1) --------------------------

#define CST3530_ADDR      0x58
#define CST3530_SDA       1
#define CST3530_SCL       3
#define CST3530_INT       4
#define CST3530_RST       2
#define PWR_LATCH_PIN     7

#define CST3530_DATA_REG      0xD0070000UL
#define CST3530_END_READ_REG  0xD00002ABUL

// The controller reports portrait 240x320 coordinates. Mapping to the
// landscape rotation=1 (320x240) the sketch draws in.
#define TOUCH_SWAP_XY   1
#define TOUCH_INVERT_X  0
#define TOUCH_INVERT_Y  1

extern bool displayFlipped;

inline void cst3530_writeReg(uint32_t reg) {
  Wire1.beginTransmission(CST3530_ADDR);
  Wire1.write((uint8_t)(reg >> 24)); Wire1.write((uint8_t)(reg >> 16));
  Wire1.write((uint8_t)(reg >> 8));  Wire1.write((uint8_t)reg);
  Wire1.endTransmission(true);
}

inline bool cst3530_readReg(uint32_t reg, uint8_t* buf, size_t len) {
  Wire1.beginTransmission(CST3530_ADDR);
  Wire1.write((uint8_t)(reg >> 24)); Wire1.write((uint8_t)(reg >> 16));
  Wire1.write((uint8_t)(reg >> 8));  Wire1.write((uint8_t)reg);
  if (Wire1.endTransmission(false) != 0) return false;
  if (Wire1.requestFrom((uint8_t)CST3530_ADDR, (uint8_t)len) != len) return false;
  for (size_t i = 0; i < len; i++) buf[i] = Wire1.read();
  return true;
}

// Reads the first touch point, mapped to screen coordinates (320x240).
inline bool cst3530_read(int* outX, int* outY) {
  uint8_t buf[9] = {0};
  if (!cst3530_readReg(CST3530_DATA_REG, buf, sizeof(buf))) return false;
  uint8_t cnt = buf[3] & 0x0F;
  bool valid = cnt > 0 && cnt <= 5 && (buf[8] & 0xF0) != 0;
  cst3530_writeReg(CST3530_END_READ_REG);
  if (!valid) return false;

  int rx = ((buf[7] & 0x0F) << 8) | buf[4];   // 0..239 (portrait)
  int ry = ((buf[7] & 0xF0) << 4) | buf[5];   // 0..319 (portrait)

  int sx = TOUCH_SWAP_XY ? ry : rx;
  int sy = TOUCH_SWAP_XY ? rx : ry;
  if (TOUCH_INVERT_X) sx = 319 - sx;
  if (TOUCH_INVERT_Y) sy = 239 - sy;

  *outX = constrain(sx, 0, 319);
  *outY = constrain(sy, 0, 239);
  if (displayFlipped) {
    *outX = 319 - *outX;
    *outY = 239 - *outY;
  }
  return true;
}

struct TouchDetail {
  bool _pressed = false;
  int  x = 0;
  int  y = 0;
  bool wasPressed() const { return _pressed; }
};

struct BoardTouch {
  TouchDetail _current;
  TouchDetail getDetail() { return _current; }
};

struct BoardSpeaker {
  // The board has an I2S DAC (PCM5101) and a speaker, but the 2.8" sketches
  // play no sound; kept silent like the CYD.
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
  LGFX_CYD&     Display = display;
  BoardTouch    Touch;
  BoardSpeaker  Speaker;

  BoardConfig config() { return BoardConfig(); }

  void begin(const BoardConfig& cfg) {
    pinMode(PWR_LATCH_PIN, OUTPUT);
    digitalWrite(PWR_LATCH_PIN, HIGH);

    display.init();
    display.setRotation(1);
    display.setBrightness(255);
    display.setColorDepth(16);
    if (cfg.clear_display) {
      display.fillScreen(TFT_BLACK);
    }

    Wire1.begin(CST3530_SDA, CST3530_SCL, 400000);
    pinMode(CST3530_INT, INPUT);
    pinMode(CST3530_RST, OUTPUT);
    digitalWrite(CST3530_RST, LOW);
    delay(100);
    digitalWrite(CST3530_RST, HIGH);
    delay(300);
  }

  void update() {
    // Poll at most ~50 Hz; the I2C transaction is cheap but not free.
    uint32_t now = millis();
    if (now - _lastPoll < 20) { Touch._current._pressed = false; return; }
    _lastPoll = now;

    int tx, ty;
    bool nowPressed = cst3530_read(&tx, &ty);
    Touch._current._pressed = (nowPressed && !_lastTouchState);
    if (nowPressed) {
      Touch._current.x = tx;
      Touch._current.y = ty;
    }
    _lastTouchState = nowPressed;
  }

private:
  bool     _lastTouchState = false;
  uint32_t _lastPoll = 0;
};

extern BoardClass Board;

inline void applyDisplayFlip(bool flip) {
  if (!flip) return;
  displayFlipped = true;
  const uint8_t r = display.getRotation();
  display.setRotation((r & 4) | ((r + 2) & 3));
  display.fillScreen(TFT_BLACK);
}
