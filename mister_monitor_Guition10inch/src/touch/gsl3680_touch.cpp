// gsl3680_touch.cpp -- Arduino wrapper for the GSL3680 touch controller.
// Modified from the vendor version:
//   * I2C goes through driver/i2c_master.h. The legacy driver/i2c.h API aborts
//     at boot on esp32 core 3.x once the new driver is in use elsewhere.
//   * No ESP_ERROR_CHECK: begin() logs the failing step and returns false, so
//     a touch fault does not take the display down with it.
//   * setRotationLandscape() maps the portrait controller onto a landscape UI.

#include "sdkconfig.h"
#include <cstring>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_gsl3680.h"
#include "gsl3680_touch.h"

#define CONFIG_LCD_HRES 800
#define CONFIG_LCD_VRES 1280

static const char *TAG = "gsl3680";

static esp_lcd_touch_handle_t     s_tp        = nullptr;
static esp_lcd_panel_io_handle_t  s_tp_io     = nullptr;
static i2c_master_bus_handle_t    s_i2c_bus   = nullptr;

static uint16_t touch_strength[1];
static uint8_t  touch_cnt = 0;

gsl3680_touch::gsl3680_touch(int8_t sda_pin, int8_t scl_pin, int8_t rst_pin, int8_t int_pin)
{
    _sda = sda_pin;
    _scl = scl_pin;
    _rst = rst_pin;
    _int = int_pin;
}

bool gsl3680_touch::begin()
{
    // Fields are assigned one by one: designated initializers must follow
    // declaration order in C++, which varies across IDF versions.
    i2c_master_bus_config_t bus_cfg;
    memset(&bus_cfg, 0, sizeof(bus_cfg));
    bus_cfg.i2c_port          = I2C_NUM_0;
    bus_cfg.sda_io_num        = (gpio_num_t)_sda;
    bus_cfg.scl_io_num        = (gpio_num_t)_scl;
    bus_cfg.clk_source        = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;
    bus_cfg.flags.enable_internal_pullup = true;

    esp_err_t r = i2c_new_master_bus(&bus_cfg, &s_i2c_bus);
    if (r == ESP_ERR_INVALID_STATE) {
        // Port already initialised elsewhere: reuse it instead of failing.
        r = i2c_master_get_bus_handle(I2C_NUM_0, &s_i2c_bus);
        ESP_LOGW(TAG, "I2C_NUM_0 already in use, reusing handle -> %s",
                 esp_err_to_name(r));
    }
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "i2c bus init failed: %s", esp_err_to_name(r));
        return false;
    }

    esp_lcd_panel_io_i2c_config_t io_cfg;
    memset(&io_cfg, 0, sizeof(io_cfg));
    io_cfg.dev_addr            = ESP_LCD_TOUCH_IO_I2C_GSL3680_ADDRESS;
    io_cfg.control_phase_bytes = 1;
    io_cfg.dc_bit_offset       = 0;
    io_cfg.lcd_cmd_bits        = 8;
    io_cfg.lcd_param_bits      = 8;
    io_cfg.flags.disable_control_phase = 1;
    io_cfg.scl_speed_hz        = 400000;

    r = esp_lcd_new_panel_io_i2c(s_i2c_bus, &io_cfg, &s_tp_io);
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "panel io i2c failed: %s", esp_err_to_name(r));
        return false;
    }

    esp_lcd_touch_config_t tp_cfg;
    memset(&tp_cfg, 0, sizeof(tp_cfg));
    tp_cfg.x_max        = CONFIG_LCD_HRES;
    tp_cfg.y_max        = CONFIG_LCD_VRES;
    tp_cfg.rst_gpio_num = (gpio_num_t)_rst;
    tp_cfg.int_gpio_num = (gpio_num_t)_int;
    tp_cfg.levels.reset     = 0;
    tp_cfg.levels.interrupt = 0;
    tp_cfg.flags.swap_xy  = 0;       // vendor defaults; see set_rotation()
    tp_cfg.flags.mirror_x = 0;
    tp_cfg.flags.mirror_y = 1;

    // Uploads the controller firmware; takes a moment.
    r = esp_lcd_touch_new_i2c_gsl3680(s_tp_io, &tp_cfg, &s_tp);
    if (r != ESP_OK) {
        ESP_LOGE(TAG, "gsl3680 init failed: %s", esp_err_to_name(r));
        return false;
    }

    ESP_LOGI(TAG, "gsl3680 ready");
    return true;
}

bool gsl3680_touch::getTouch(uint16_t *x, uint16_t *y)
{
    if (s_tp == nullptr) return false;
    esp_lcd_touch_read_data(s_tp);
    return esp_lcd_touch_get_coordinates(s_tp, x, y, touch_strength, &touch_cnt, 1);
}

// Portrait-native rotations, as the vendor defined them.
void gsl3680_touch::set_rotation(uint8_t r)
{
    if (s_tp == nullptr) return;
    switch (r) {
    case 0:
    case 2:
        esp_lcd_touch_set_swap_xy (s_tp, false);
        esp_lcd_touch_set_mirror_x(s_tp, false);
        esp_lcd_touch_set_mirror_y(s_tp, false);
        break;
    case 1:
    case 3:
        esp_lcd_touch_set_swap_xy (s_tp, false);
        esp_lcd_touch_set_mirror_x(s_tp, true);
        esp_lcd_touch_set_mirror_y(s_tp, true);
        break;
    }
}

// Landscape mapping: the controller is portrait (800x1280), the UI is 1280x800.
void gsl3680_touch::setRotationLandscape(bool mirror_x, bool mirror_y)
{
    if (s_tp == nullptr) return;
    esp_lcd_touch_set_swap_xy (s_tp, true);
    esp_lcd_touch_set_mirror_x(s_tp, mirror_x);
    esp_lcd_touch_set_mirror_y(s_tp, mirror_y);
}
