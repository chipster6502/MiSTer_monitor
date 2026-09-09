// gsl3680_touch.h -- Arduino wrapper for the GSL3680 touch controller.
// Modified from the vendor version:
//   * begin() returns bool instead of aborting on failure
//   * uses the current ESP-IDF I2C master driver
//   * setRotationLandscape() added: the vendor rotation never swaps axes, so it
//     cannot map the portrait controller onto a landscape display
#ifndef _GSL3680_TOUCH_H
#define _GSL3680_TOUCH_H
#include <stdio.h>
#include <stdint.h>

class gsl3680_touch
{
public:
    gsl3680_touch(int8_t sda_pin, int8_t scl_pin, int8_t rst_pin = -1, int8_t int_pin = -1);

    bool begin();                                  // false on failure, no abort
    bool getTouch(uint16_t *x, uint16_t *y);
    void set_rotation(uint8_t r);                  // portrait-native
    void setRotationLandscape(bool mirror_x = true, bool mirror_y = false);

private:
    int8_t _sda, _scl, _rst, _int;
};

#endif
