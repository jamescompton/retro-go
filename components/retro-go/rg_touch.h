#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef RG_TOUCH_CS
void rg_touch_init(void);
void rg_touch_deinit(void);
uint32_t rg_touch_read(void);
void rg_touch_blend(uint16_t *pixels, int left, int top, int width, int height);
bool rg_touch_enabled(void);
bool rg_touch_input_enabled(void);
bool rg_touch_set_input_enabled(bool enabled);
int rg_touch_colour(void);
int rg_touch_opacity(void);
bool rg_touch_labels(void);
bool rg_touch_set_labels(bool show);
bool rg_touch_configure(bool enabled, int colour, int opacity);
bool rg_touch_calibrated(void);
bool rg_touch_calibrate(void);
void rg_touch_recover(void);
#else
#define rg_touch_init()
#define rg_touch_deinit()
#define rg_touch_read() 0
#define rg_touch_blend(pixels, left, top, width, height)
#endif
