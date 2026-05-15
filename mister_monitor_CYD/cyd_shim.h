// ============================================================================
//  cyd_shim.h — M5Unified compatibility layer for the Cheap Yellow Display
//
//  This header lets the MiSTer Monitor sketch — originally written against
//  the M5Unified API for the M5Stack Tab5 — compile and run on the
//  ESP32-2432S028R "CYD" board with no changes to the drawing logic.
//
//  It provides a global `M5` object whose interface matches the subset of
//  M5Unified used by the sketch (Display, Touch, Speaker, config, begin,
//  update). Display calls are routed to a LovyanGFX instance configured
//  for the CYD's ILI9341 panel; Touch is currently a stub (XPT2046 wiring
//  will be added in a later step); Speaker is a no-op (CYD has no DAC speaker).
//
//  Pinout reference (CYD ESP32-2432S028R, classic XPT2046 variant):
//    TFT (VSPI):  SCK=14  MOSI=13  MISO=12  DC=2  CS=15  RST=hw  BL=21
//    SD  (HSPI):  SCK=18  MOSI=23  MISO=19  CS=5     (handled in .ino, not here)
//    Touch (VSPI shared, separate CS): CS=33  IRQ=36  (wired up later)
// ============================================================================

#pragma once

#include <LovyanGFX.hpp>

// ---------- LovyanGFX panel class for the CYD ------------------------------

class LGFX_CYD : public lgfx::LGFX_Device {
  lgfx::Panel_ILI9341 _panel_instance;
  lgfx::Bus_SPI       _bus_instance;
  lgfx::Light_PWM     _light_instance;

public:
  LGFX_CYD(void) {
    {
      auto cfg = _bus_instance.config();
      cfg.spi_host    = SPI3_HOST;        // VSPI on ESP32 classic
      cfg.spi_mode    = 0;
      cfg.freq_write  = 40000000;         // 40 MHz; drop to 27000000 if you see glitches
      cfg.freq_read   = 16000000;
      cfg.spi_3wire   = false;
      cfg.use_lock    = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = 14;
      cfg.pin_mosi = 13;
      cfg.pin_miso = 12;
      cfg.pin_dc   = 2;
      _bus_instance.config(cfg);
      _panel_instance.setBus(&_bus_instance);
    }
    {
      auto cfg = _panel_instance.config();
      cfg.pin_cs           = 15;
      cfg.pin_rst          = -1;          // CYD ties RST to EN; no MCU pin
      cfg.pin_busy         = -1;
      cfg.panel_width      = 240;
      cfg.panel_height     = 320;
      cfg.offset_x         = 0;
      cfg.offset_y         = 0;
      cfg.offset_rotation  = 0;
      cfg.dummy_read_pixel = 8;
      cfg.dummy_read_bits  = 1;
      cfg.readable         = true;
      cfg.invert           = false;
      cfg.rgb_order        = false;       // BGR on most CYD batches; flip to true if R/B swap
      cfg.dlen_16bit       = false;
      cfg.bus_shared       = true;        // shared with SD/touch on VSPI
      _panel_instance.config(cfg);
    }
    {
      auto cfg = _light_instance.config();
      cfg.pin_bl      = 21;
      cfg.invert      = false;
      cfg.freq        = 44100;
      cfg.pwm_channel = 7;
      _light_instance.config(cfg);
      _panel_instance.setLight(&_light_instance);
    }
    setPanel(&_panel_instance);
  }
};

// Single global display instance used by the M5 shim below.
extern LGFX_CYD display;

// ---------- M5Unified-compatible facade ------------------------------------
//
//  Only the API surface actually used by the sketch is implemented. If a
//  future change touches a new M5 method, add it here rather than in the
//  sketch — that keeps the port surgical.

struct M5_TouchDetail {
  bool _pressed = false;
  int  x = 0;
  int  y = 0;
  bool wasPressed() const { return _pressed; }
};

struct M5_Touch {
  // STUB: real XPT2046 polling will be added later.
  // Returning "no touch" keeps the interface usable but inert.
  M5_TouchDetail getDetail() { return M5_TouchDetail(); }
};

struct M5_Speaker {
  // No-op: CYD has no DAC-driven speaker. A passive buzzer port may be
  // added later but is intentionally silent for now.
  void begin() {}
  void setVolume(int) {}
  void tone(int /*freq*/, int /*duration_ms*/) {}
};

struct M5_Config {
  bool clear_display = true;
  bool output_power  = true;
  bool internal_imu  = false;
  bool external_imu  = false;
};

struct M5_Stub {
  LGFX_CYD&   Display = display;
  M5_Touch    Touch;
  M5_Speaker  Speaker;

  M5_Config config() { return M5_Config(); }

  void begin(const M5_Config& cfg) {
    display.init();
    display.setRotation(1);          // landscape, USB on the right
    display.setBrightness(255);
    display.setColorDepth(16);
    if (cfg.clear_display) {
      display.fillScreen(TFT_BLACK);
    }
  }

  void update() {
    // STUB: will poll XPT2046 in Step 4.
  }
};

extern M5_Stub M5;
