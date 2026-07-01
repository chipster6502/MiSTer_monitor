// ============================================================================
//  Compatibility layer for the ESP32-3248S035R ("CYD" 3.5" RESISTIVE)
//
//  This is the resistive sibling of the ESP32-3248S035C port. The display is
//  identical (ST7796, 480x320 landscape); only the touch differs: this board
//  uses an XPT2046 resistive controller on the SAME SPI bus as the panel,
//  instead of the GT911 capacitive controller (I2C) of the 035C.
//
//  The class name (LGFX_CYD35) and the Board facade are kept identical to the
//  035C on purpose, so this folder can reuse the very same .ino and
//  board_hal.cpp — only THIS header differs between the two variants.
//
//  Confirmed pinout (ESP32-3248S035R):
//    TFT   (SPI):  SCK=14  MOSI=13  MISO=12  DC=2  CS=15  RST=EN(-1)  BL=27
//    Touch (SPI):  SCLK=14 MOSI=13  MISO=12  CS=33  IRQ=36  (XPT2046, SHARED bus)
//    SD    (HSPI): SCK=18  MOSI=23  MISO=19  CS=5            (handled in .ino)
//    RGB LED:      R=4  G=16  B=17  (active-low)
//    Audio amp:    GPIO26  (unused here)
//
// ============================================================================

#pragma once

#include <LovyanGFX.hpp>

// ---------- LovyanGFX panel + touch class for the ESP32-3248S035R ----------

class LGFX_CYD35 : public lgfx::LGFX_Device {
  lgfx::Panel_ST7796   _panel_instance;
  lgfx::Bus_SPI        _bus_instance;
  lgfx::Light_PWM      _light_instance;
  lgfx::Touch_XPT2046  _touch_instance;   // resistive (was Touch_GT911 on the 035C)

public:
  LGFX_CYD35(void) {
    // ----- SPI bus (shared VSPI: TFT + SD + XPT2046 touch) -----
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
      cfg.bus_shared       = true;        // shared with SD + touch on this bus
      _panel_instance.config(cfg);
    }
    // ----- Backlight (PWM on GPIO27) -----
    {
      auto cfg = _light_instance.config();
      cfg.pin_bl      = 27;               // 035 backlight (not 21 like the 2.8")
      cfg.invert      = false;
      cfg.freq        = 44100;
      cfg.pwm_channel = 7;
      _light_instance.config(cfg);
      _panel_instance.setLight(&_light_instance);
    }
    // ----- XPT2046 resistive touch (shares the panel's SPI bus) -----
    {
      auto cfg = _touch_instance.config();
      // XPT2046 returns RAW 12-bit ADC values; these min/max map them to screen
      // coordinates. They are STARTING ESTIMATES for a 480x320 ST7796 panel and
      // will very likely need refining on the real unit (resistive panels are
      // not pre-aligned). If touch responds but lands in the wrong place,
      // capture the raw values at the four corners and adjust these; if the axes
      // come out swapped or mirrored, also try cfg.offset_rotation = 1/2/3.
      cfg.x_min   = 300;
      cfg.x_max   = 3900;
      cfg.y_min   = 300;
      cfg.y_max   = 3900;
      cfg.pin_int = 36;                   // IRQ
      cfg.pin_cs  = 33;
      cfg.spi_host = SPI3_HOST;           // SAME SPI bus as the panel (shared)
      cfg.pin_sclk = 14;                  // shared with TFT
      cfg.pin_mosi = 13;                  // shared with TFT
      cfg.pin_miso = 12;                  // shared with TFT
      cfg.freq    = 1000000;              // XPT2046 reads slow (<=2.5 MHz); 1 MHz is safe
      cfg.bus_shared      = true;         // arbitrate with the 40 MHz panel on the same bus
      cfg.offset_rotation = 2;            // try 1/2/3 if axes end up swapped/mirrored
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
  // No-op: kept silent. The 035 has an audio amp on GPIO26, but for now the port
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
    display.init();              // brings up panel + XPT2046 over the shared SPI bus
    display.setRotation(1);      // 480x320 landscape
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
