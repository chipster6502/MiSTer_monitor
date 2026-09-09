// MiSTer Monitor
// Copyright (C) 2025-2026 chipster6502
// SPDX-License-Identifier: GPL-3.0-or-later

// =============================================================================
//  Panel_JD9365.hpp -- 800x1280 MIPI-DSI panel (JD9365) for lgfx on ESP32-P4
//
//  The panel is brought up by the vendor's esp_lcd driver (src/lcd/, verbatim,
//  including its init table). lgfx only renders: Panel_JD9365 derives from
//  Panel_FrameBufferBase and receives the DPI framebuffer as an array of row
//  pointers, so the normal drawing API works on top of it unchanged.
//
//  lgfx::Panel_DSI is deliberately not used. It is written for other DSI panels
//  and creates the DPI layer with a different framebuffer count than this
//  panel's configuration; with it the panel only ever shows its internal test
//  pattern while init() still reports success.
//
//  UI window: the sketch is laid out for a 1280x720 canvas. The panel exposes
//  a 720-pixel-wide window centred on its 800-pixel portrait width, which after
//  rotation 1 gives 1280x720 landscape with a 40-pixel black band top and
//  bottom. Set JD9365_UI_INSET to 0 for the full 1280x800 canvas.
//
//  The DPI framebuffer is DMA-coherent: no cache flush is needed after drawing.
// =============================================================================

#pragma once

#include <sdkconfig.h>
#if defined(CONFIG_IDF_TARGET_ESP32P4)

#include <cstring>
#include <esp_heap_caps.h>
#include <esp_ldo_regulator.h>
#include <esp_lcd_mipi_dsi.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <driver/ledc.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <lgfx/v1/panel/Panel_FrameBufferBase.hpp>

#include "src/lcd/esp_lcd_jd9365.h"

// ---- board wiring -----------------------------------------------------------
#ifndef JD9365_PIN_LCD_RST
  #define JD9365_PIN_LCD_RST   27    // active low
#endif
#ifndef JD9365_PIN_BL
  #define JD9365_PIN_BL        23    // backlight, PWM, active high
#endif
#ifndef JD9365_BL_LEDC_CH
  #define JD9365_BL_LEDC_CH    LEDC_CHANNEL_0
#endif
#ifndef JD9365_BL_LEDC_TIMER
  #define JD9365_BL_LEDC_TIMER LEDC_TIMER_0
#endif
#ifndef JD9365_BL_FREQ_HZ
  #define JD9365_BL_FREQ_HZ    5000
#endif

// MIPI D-PHY power: internal LDO channel 3 @ 2500 mV, before the DSI bus.
#ifndef JD9365_LDO_DPHY_CHAN
  #define JD9365_LDO_DPHY_CHAN  3
#endif
#ifndef JD9365_LDO_DPHY_MV
  #define JD9365_LDO_DPHY_MV    2500
#endif

// Pixels hidden on each side of the portrait width (top/bottom in landscape).
#ifndef JD9365_UI_INSET
  #define JD9365_UI_INSET       40
#endif

// Set to 1 to log every bring-up step with its esp_err_t.
#ifndef JD9365_VERBOSE
  #define JD9365_VERBOSE 0
#endif
#if JD9365_VERBOSE
  #define JD9365_LOG(fmt, ...) Serial.printf("[jd9365] " fmt "\n", ##__VA_ARGS__)
#else
  #define JD9365_LOG(fmt, ...) do {} while (0)
#endif

namespace m5gfx_user {

struct Panel_JD9365 : public lgfx::v1::Panel_FrameBufferBase
{
  static constexpr uint16_t PANEL_W = 800;    // physical panel is portrait
  static constexpr uint16_t PANEL_H = 1280;
  static constexpr uint16_t UI_W    = PANEL_W - 2 * JD9365_UI_INSET;

  Panel_JD9365()
  {
    _cfg.memory_width  = _cfg.panel_width  = UI_W;
    _cfg.memory_height = _cfg.panel_height = PANEL_H;
    _cfg.offset_x = 0;
    _cfg.offset_y = 0;
  }

  // MIPI DPI expects native byte order, not the swapped order of SPI panels.
  lgfx::v1::color_depth_t setColorDepth(lgfx::v1::color_depth_t) override
  {
    _write_depth = lgfx::v1::color_depth_t::rgb565_nonswapped;
    _read_depth  = lgfx::v1::color_depth_t::rgb565_nonswapped;
    return _write_depth;
  }

  bool init(bool use_reset) override
  {
    if (_lines_buffer != nullptr) return false;      // already initialised
    setColorDepth(lgfx::v1::color_depth_t::rgb565_nonswapped);

    if (!lgfx::v1::Panel_FrameBufferBase::init(use_reset)) {
      JD9365_LOG("Panel_FrameBufferBase::init FAILED");
      return false;
    }
    if (!_dphyPower())   { JD9365_LOG("LDO power FAILED");    return false; }
    if (!_bringUp())     { JD9365_LOG("DSI bring-up FAILED"); return false; }
    if (!_mapLines())    { JD9365_LOG("line map FAILED");     return false; }
    _initBacklight();
    JD9365_LOG("panel ready");
    return true;
  }

  // No display() override: the framebuffer is DMA-coherent.

  void*  framebuffer(void)      const { return _fb; }
  size_t framebufferBytes(void) const { return _fbBytes(); }
  esp_lcd_panel_handle_t dpiHandle(void) const { return _panel; }

  // 0..255, as elsewhere in lgfx.
  void setBrightness(uint8_t brightness) override
  {
    ledc_set_duty(LEDC_LOW_SPEED_MODE, JD9365_BL_LEDC_CH, brightness);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, JD9365_BL_LEDC_CH);
  }

private:
  esp_lcd_dsi_bus_handle_t  _bus   = nullptr;
  esp_lcd_panel_io_handle_t _io    = nullptr;
  esp_lcd_panel_handle_t    _panel = nullptr;
  esp_ldo_channel_handle_t  _ldo   = nullptr;
  void*                     _fb    = nullptr;

  size_t _fbBytes(void) const
  {
    return (size_t)PANEL_W * PANEL_H * 2;   // RGB565
  }

  bool _dphyPower(void)
  {
    esp_ldo_channel_config_t cfg = {};
    cfg.chan_id    = JD9365_LDO_DPHY_CHAN;
    cfg.voltage_mv = JD9365_LDO_DPHY_MV;
    esp_err_t r = esp_ldo_acquire_channel(&cfg, &_ldo);
    JD9365_LOG("LDO ch%d %dmV -> %s", JD9365_LDO_DPHY_CHAN, JD9365_LDO_DPHY_MV,
               esp_err_to_name(r));
    return r == ESP_OK;
  }

  // Vendor sequence: bus -> DBI IO -> DPI panel -> JD9365 -> reset -> init.
  bool _bringUp(void)
  {
    esp_lcd_dsi_bus_config_t bus_cfg = JD9365_PANEL_BUS_DSI_2CH_CONFIG();
    esp_err_t r = esp_lcd_new_dsi_bus(&bus_cfg, &_bus);
    JD9365_LOG("dsi bus (%d lanes @ %d Mbps) -> %s", bus_cfg.num_data_lanes,
               (int)bus_cfg.lane_bit_rate_mbps, esp_err_to_name(r));
    if (r != ESP_OK) return false;

    esp_lcd_dbi_io_config_t dbi_cfg = JD9365_PANEL_IO_DBI_CONFIG();
    r = esp_lcd_new_panel_io_dbi(_bus, &dbi_cfg, &_io);
    JD9365_LOG("dbi io -> %s", esp_err_to_name(r));
    if (r != ESP_OK) return false;

    esp_lcd_dpi_panel_config_t dpi_cfg =
        JD9365_800_1280_PANEL_60HZ_DPI_CONFIG(LCD_COLOR_PIXEL_FORMAT_RGB565);

    jd9365_vendor_config_t vendor_cfg = {};
    vendor_cfg.init_cmds      = nullptr;   // built-in init table
    vendor_cfg.init_cmds_size = 0;
    vendor_cfg.mipi_config.dsi_bus    = _bus;
    vendor_cfg.mipi_config.dpi_config = &dpi_cfg;
    vendor_cfg.mipi_config.lane_num   = 2;

    esp_lcd_panel_dev_config_t dev_cfg = {};
    dev_cfg.reset_gpio_num = JD9365_PIN_LCD_RST;
    dev_cfg.rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB;
    dev_cfg.bits_per_pixel = 16;
    dev_cfg.vendor_config  = &vendor_cfg;

    r = esp_lcd_new_panel_jd9365(_io, &dev_cfg, &_panel);
    JD9365_LOG("new_panel_jd9365 -> %s", esp_err_to_name(r));
    if (r != ESP_OK) return false;

    r = esp_lcd_panel_reset(_panel);
    JD9365_LOG("panel_reset -> %s", esp_err_to_name(r));
    if (r != ESP_OK) return false;

    r = esp_lcd_panel_init(_panel);       // runs the vendor init table
    JD9365_LOG("panel_init -> %s", esp_err_to_name(r));
    if (r != ESP_OK) return false;

    r = esp_lcd_dpi_panel_get_frame_buffer(_panel, 1, &_fb);
    JD9365_LOG("get_frame_buffer -> %s  fb=%p (%u bytes)",
               esp_err_to_name(r), _fb, (unsigned)_fbBytes());
    if (r != ESP_OK || _fb == nullptr) return false;

    // Whole framebuffer, including the hidden bands, starts black.
    memset(_fb, 0, _fbBytes());
    return true;
  }

  // Hand the UI window of each framebuffer row to lgfx as a row pointer.
  bool _mapLines(void)
  {
    const size_t la_size = (size_t)PANEL_H * sizeof(uint8_t*);
    uint8_t** lineArray = (uint8_t**)heap_caps_malloc(la_size, MALLOC_CAP_DMA);
    if (lineArray == nullptr) return false;
    memset(lineArray, 0, la_size);

    // Physical row stride (whole panel width), 4-byte aligned like lgfx does.
    const size_t line_length = (((size_t)PANEL_W * _write_bits >> 3) + 3) & ~3u;
    const size_t inset_bytes = (size_t)JD9365_UI_INSET * (_write_bits >> 3);
    uint8_t* ptr = (uint8_t*)_fb + inset_bytes;
    for (int y = 0; y < PANEL_H; y++) { lineArray[y] = ptr; ptr += line_length; }
    _lines_buffer = lineArray;

    JD9365_LOG("line map ok (stride %u bytes, inset %u px)",
               (unsigned)line_length, (unsigned)JD9365_UI_INSET);
    return true;
  }

  void _initBacklight(void)
  {
    ledc_timer_config_t t = {};
    t.speed_mode      = LEDC_LOW_SPEED_MODE;
    t.duty_resolution = LEDC_TIMER_8_BIT;
    t.timer_num       = JD9365_BL_LEDC_TIMER;
    t.freq_hz         = JD9365_BL_FREQ_HZ;
    t.clk_cfg         = LEDC_AUTO_CLK;
    ledc_timer_config(&t);

    ledc_channel_config_t c = {};
    c.gpio_num   = JD9365_PIN_BL;
    c.speed_mode = LEDC_LOW_SPEED_MODE;
    c.channel    = JD9365_BL_LEDC_CH;
    c.timer_sel  = JD9365_BL_LEDC_TIMER;
    c.duty       = 0;
    c.hpoint     = 0;
    ledc_channel_config(&c);
  }
};

// -----------------------------------------------------------------------------
//  Device class. Rotation 1 turns the portrait panel into the landscape canvas
//  the UI expects. The JD9365 has no hardware 90/270 rotation, so this is a
//  software transform applied per draw call; measured cost is negligible for
//  this sketch's mostly static content.
// -----------------------------------------------------------------------------
class LGFX_JC8012P4A1C : public lgfx::LGFX_Device
{
  Panel_JD9365 _panel;

public:
  LGFX_JC8012P4A1C() { setPanel(&_panel); }

  Panel_JD9365& panel(void) { return _panel; }

  bool init_impl(bool use_reset, bool use_clear) override
  {
    bool ok = lgfx::LGFX_Device::init_impl(use_reset, use_clear);
    setRotation(1);
    return ok;
  }
};

} // namespace m5gfx_user

#endif // CONFIG_IDF_TARGET_ESP32P4
