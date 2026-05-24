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

// ---------- XPT2046 resistive touch (software SPI / bit-bang) --------------
//
//  The CYD touch controller is wired to its own pin set, but the ESP32's
//  two hardware SPI controllers are already taken (VSPI=TFT, HSPI=SD).
//  The XPT2046 is very low bandwidth so a software-SPI read is more than
//  fast enough and avoids any bus contention.

#define XPT2046_CLK   25
#define XPT2046_MOSI  32
#define XPT2046_MISO  39
#define XPT2046_CS    33
#define XPT2046_IRQ   36

// Raw-ADC -> screen calibration for landscape rotation=1 (USB on the right).
// These are typical CYD values; refine them with the calibration mode in
// Step 9.4 if touches land off-target.
#define TS_RAW_X_MIN  850      // physical BOTTOM of screen
#define TS_RAW_X_MAX  6600     // physical TOP of screen
#define TS_RAW_Y_MIN  1350     // physical LEFT
#define TS_RAW_Y_MAX  7170     // physical RIGHT

// Read one 12-bit channel from the XPT2046 by bit-banging the SPI clock.
inline uint16_t xpt2046_readChannel(uint8_t cmd) {
  digitalWrite(XPT2046_CS, LOW);

  // Send 8-bit command, MSB first
  for (int i = 7; i >= 0; i--) {
    digitalWrite(XPT2046_MOSI, (cmd >> i) & 0x01);
    digitalWrite(XPT2046_CLK, HIGH);
    digitalWrite(XPT2046_CLK, LOW);
  }

  // Read 16 bits; the 12-bit result sits in bits [15:3]
  uint16_t value = 0;
  for (int i = 0; i < 16; i++) {
    digitalWrite(XPT2046_CLK, HIGH);
    digitalWrite(XPT2046_CLK, LOW);
    value = (value << 1) | (digitalRead(XPT2046_MISO) & 0x01);
  }

  digitalWrite(XPT2046_CS, HIGH);
  return value >> 3;          // keep 12 significant bits
}

// Median-of-5 read for noise rejection. Returns false if no touch.
inline bool xpt2046_read(int* outX, int* outY) {
  // IRQ is LOW while the panel is being pressed
  if (digitalRead(XPT2046_IRQ) == HIGH) return false;

  uint16_t xs[5], ys[5];
  for (int i = 0; i < 5; i++) {
    xs[i] = xpt2046_readChannel(0xD0);  // X channel
    ys[i] = xpt2046_readChannel(0x90);  // Y channel
  }
  // Simple insertion sort, take the middle sample
  for (int i = 1; i < 5; i++) {
    uint16_t kx = xs[i], ky = ys[i];
    int j = i - 1;
    while (j >= 0 && xs[j] > kx) { xs[j+1] = xs[j]; j--; } xs[j+1] = kx;
    j = i - 1;
    while (j >= 0 && ys[j] > ky) { ys[j+1] = ys[j]; j--; } ys[j+1] = ky;
  }
  uint16_t rawX = xs[2];
  uint16_t rawY = ys[2];

  if (rawX < 100 || rawY < 100) return false;   // spurious / release

  // Landscape rotation=1: screen X from raw Y, screen Y from raw X.
  // BOTH axes are inverted on this panel relative to the raw ADC range,
  // so both map() calls have their input endpoints swapped:
  //   physical left  -> low  raw Y -> screen x = 0
  //   physical bottom-> low  raw X -> screen y = 239
  long sx = map(rawY, TS_RAW_Y_MAX, TS_RAW_Y_MIN, 0, 320);
  long sy = map(rawX, TS_RAW_X_MAX, TS_RAW_X_MIN, 0, 240);

  *outX = constrain(sx, 0, 319);
  *outY = constrain(sy, 0, 239);

  // CALIBRATION LOG: remove once TS_RAW_*_MIN/MAX are confirmed
  Serial.printf("RAW: x=%d y=%d  ->  screen=(%d,%d)\n",
                rawX, rawY, *outX, *outY);

  return true;
}

struct M5_TouchDetail {
  bool _pressed = false;
  int  x = 0;
  int  y = 0;
  bool wasPressed() const { return _pressed; }
};

struct M5_Touch {
  // Latest polled state, refreshed by M5.update().
  M5_TouchDetail _current;
  M5_TouchDetail getDetail() { return _current; }
};

// ---------- M5Unified-compatible facade ------------------------------------
//
//  Only the API surface actually used by the sketch is implemented. If a
//  future change touches a new M5 method, add it here rather than in the
//  sketch — that keeps the port surgical.
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
    display.setRotation(1);
    display.setBrightness(255);
    display.setColorDepth(16);
    if (cfg.clear_display) {
      display.fillScreen(TFT_BLACK);
    }

    // XPT2046 bit-bang pins
    pinMode(XPT2046_CLK,  OUTPUT);
    pinMode(XPT2046_MOSI, OUTPUT);
    pinMode(XPT2046_CS,   OUTPUT);
    pinMode(XPT2046_MISO, INPUT);
    pinMode(XPT2046_IRQ,  INPUT);
    digitalWrite(XPT2046_CS,  HIGH);
    digitalWrite(XPT2046_CLK, LOW);
  }

  void update() {
    int tx, ty;
    bool nowPressed = xpt2046_read(&tx, &ty);

    // wasPressed() == true for exactly one update cycle on press edge,
    // matching M5Unified semantics the sketch relies on.
    Touch._current._pressed = (nowPressed && !_lastTouchState);
    if (nowPressed) {
      Touch._current.x = tx;
      Touch._current.y = ty;
    }
    _lastTouchState = nowPressed;
  }

private:
  bool _lastTouchState = false;
public: 
};

extern M5_Stub M5;
