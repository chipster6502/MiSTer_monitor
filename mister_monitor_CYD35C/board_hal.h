// ============================================================================
//  Compatibility layer for the ESP32-3248S035C ("CYD" 3.5" capacitive)
//
//  This header lets the MiSTer Monitor sketch compile and run on the
//  ESP32-3248S035C board with no changes to the drawing logic. It mirrors
//  the contract of the ESP32-2432S028R port (same `display` + `Board`
//  facade), so the .ino remains hardware-agnostic.
//
//  Differences vs. the 2.8" CYD baseline:
//    - Panel:     ST7796  (native 320x480 -> 480x320 landscape), not ILI9341
//    - Touch:     GT911 capacitive over I2C, not XPT2046 resistive over SPI
//    - Backlight: GPIO27, not GPIO21
//
//  Confirmed pinout (ESP32-3248S035C):
//    TFT  (VSPI):  SCK=14  MOSI=13  MISO=12  DC=2  CS=15  RST=EN(-1)  BL=27
//    Touch (I2C):  SDA=33  SCL=32   INT=21*  RST=25*        (GT911, addr 0x5D)
//    SD   (HSPI):  SCK=18  MOSI=23  MISO=19  CS=5           (handled in .ino)
//    RGB LED:      R=4  G=16  B=17  (active-low)
//    Audio amp:    GPIO26  (unused here)
//
//    * INT(21)/RST(25) are unreliable on this board: pin 21 is multiplexed
//      and pin 25 is tied to the ESP32 reset line. Both are left at -1 so the
//      GT911 runs in pure I2C polling mode with a fixed address. This is the
//      configuration known to work on these panels. If a future batch wires
//      them properly, enabling them gives lower-latency touch.
// ============================================================================

#pragma once

#include <LovyanGFX.hpp>

// ---------- LovyanGFX panel + touch class for the ESP32-3248S035C ----------

class LGFX_CYD35 : public lgfx::LGFX_Device {
  lgfx::Panel_ST7796  _panel_instance;
  lgfx::Bus_SPI       _bus_instance;
  lgfx::Light_PWM     _light_instance;
  lgfx::Touch_GT911   _touch_instance;

public:
  LGFX_CYD35(void) {
    // ----- SPI bus (shared VSPI: TFT + SD) -----
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
    // ----- ST7796 panel (native 320x480 portrait) -----
    {
      auto cfg = _panel_instance.config();
      cfg.pin_cs           = 15;
      cfg.pin_rst          = -1;          // tied to EN; no MCU pin
      cfg.pin_busy         = -1;
      cfg.panel_width      = 320;         // native portrait width
      cfg.panel_height     = 480;         // native portrait height
      cfg.offset_x         = 0;
      cfg.offset_y         = 0;
      cfg.offset_rotation  = 0;
      cfg.dummy_read_pixel = 8;
      cfg.dummy_read_bits  = 1;
      cfg.readable         = true;
      cfg.invert           = false;       // ST7796: if colors look negative, set true
      cfg.rgb_order        = false;       // if red/blue are swapped, set true
      cfg.dlen_16bit       = false;
      cfg.bus_shared       = true;        // shared with SD on VSPI
      _panel_instance.config(cfg);
    }
    // ----- Backlight (PWM on GPIO27) -----
    {
      auto cfg = _light_instance.config();
      cfg.pin_bl      = 27;               // 035C backlight (not 21 like the 2.8")
      cfg.invert      = false;
      cfg.freq        = 44100;
      cfg.pwm_channel = 7;
      _light_instance.config(cfg);
      _panel_instance.setLight(&_light_instance);
    }
    // ----- GT911 capacitive touch (I2C) -----
    {
      auto cfg = _touch_instance.config();
      // Raw coordinate range in the panel's NATIVE (portrait) orientation.
      // setRotation(1) in begin() rotates touch to match 480x320 landscape.
      cfg.x_min       = 0;
      cfg.x_max       = 319;
      cfg.y_min       = 0;
      cfg.y_max       = 479;
      cfg.pin_int     = -1;               // polling mode (INT unreliable on 035C)
      cfg.pin_rst     = -1;               // RST tied to board reset
      cfg.bus_shared  = false;            // its own I2C bus, not the SPI bus
      cfg.offset_rotation = 0;            // if axes end up swapped/mirrored, try 1/2/3
      cfg.i2c_port    = 0;                // I2C_NUM_0
      cfg.i2c_addr    = 0x5D;             // GT911 default; some report 0x14
      cfg.pin_sda     = 33;
      cfg.pin_scl     = 32;
      cfg.freq        = 400000;
      _touch_instance.config(cfg);
      _panel_instance.setTouch(&_touch_instance);
    }
    setPanel(&_panel_instance);
  }
};

extern LGFX_CYD35 display;

// ---------- Onboard RGB status LED (active-low) ----------------------------
// Wired to GPIO 4/16/17. Not part of the Board facade; we only force it OFF
// at boot so it does not glow. Drive a pin LOW to light that channel.
#define CYD35_LED_R  4
#define CYD35_LED_G  16
#define CYD35_LED_B  17

// ---------- Touch state structs (identical contract to the 2.8" port) ------

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
  // No-op: kept silent. The 035C has an audio amp on GPIO26, but for now the port
  // does not drive sound. Implement here later if a buzzer/tone is wanted (as the Tab5 has).
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
  LGFX_CYD35&   Display = display;
  BoardTouch    Touch;
  BoardSpeaker  Speaker;

  BoardConfig config() { return BoardConfig(); }

  void begin(const BoardConfig& cfg) {
    display.init();              // brings up panel + GT911 over I2C
    display.setRotation(1);      // 480x320 landscape (USB on the right)
    display.setBrightness(255);
    display.setColorDepth(16);
    if (cfg.clear_display) {
      display.fillScreen(TFT_BLACK);
    }

    // Force the onboard RGB LED off (active-low -> HIGH = off).
    pinMode(CYD35_LED_R, OUTPUT); digitalWrite(CYD35_LED_R, HIGH);
    pinMode(CYD35_LED_G, OUTPUT); digitalWrite(CYD35_LED_G, HIGH);
    pinMode(CYD35_LED_B, OUTPUT); digitalWrite(CYD35_LED_B, HIGH);
  }

  void update() {
    int32_t tx = 0, ty = 0;
    bool nowPressed = display.getTouch(&tx, &ty);   // already in 480x320 coords

    // wasPressed() == true for exactly one update cycle on the press edge.
    Touch._current._pressed = (nowPressed && !_lastTouchState);
    if (nowPressed) {
      Touch._current.x = (int)tx;
      Touch._current.y = (int)ty;
    }
    _lastTouchState = nowPressed;
  }

private:
  bool _lastTouchState = false;
public:
};

extern BoardClass Board;
