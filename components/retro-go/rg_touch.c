#include "rg_system.h"
#include "rg_touch.h"

#ifdef RG_TOUCH_CS
#include <driver/spi_master.h>
#include <driver/gpio.h>
#include <math.h>
#include <string.h>

typedef struct
{
    int x, y, w, h;
    uint32_t key;
    char glyph;
} touch_button_t;

static const touch_button_t buttons[] = {
    {52, 188, 36, 36, RG_KEY_UP, '^'}, {52, 260, 36, 36, RG_KEY_DOWN, 'v'},
    {16, 224, 36, 36, RG_KEY_LEFT, '<'}, {88, 224, 36, 36, RG_KEY_RIGHT, '>'},
    {392, 188, 36, 36, RG_KEY_Y, 'Y'}, {392, 260, 36, 36, RG_KEY_A, 'A'},
    {356, 224, 36, 36, RG_KEY_X, 'X'}, {428, 224, 36, 36, RG_KEY_B, 'B'},
    {0, 160, 48, 28, RG_KEY_L, 'L'}, {432, 160, 48, 28, RG_KEY_R, 'R'},
    {164, 284, 56, 32, RG_KEY_SELECT, '-'}, {260, 284, 56, 32, RG_KEY_START, '+'},
    {0, 0, 56, 32, RG_KEY_MENU, 'M'}, {424, 0, 56, 32, RG_KEY_OPTION, 'O'},
};
static const uint16_t colours[] = {C_WHITE, C_RED, C_GREEN, C_BLUE, C_YELLOW};
static spi_device_handle_t touch_spi;
static portMUX_TYPE touch_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile bool enabled, calibrated, calibrating, show_labels, ready, release_pending;
static volatile bool input_enabled;
static volatile int colour_index, opacity;
static float affine[6];
static int raw_x, raw_y;
static bool pressed;

bool rg_touch_enabled(void) { return enabled; }
bool rg_touch_input_enabled(void) { return input_enabled; }

bool rg_touch_set_input_enabled(bool on)
{
    release_pending = true;
    input_enabled = on;
    rg_settings_set_boolean(NS_GLOBAL, "TouchInput", on);
    return rg_settings_commit();
}
int rg_touch_colour(void) { return colour_index; }
int rg_touch_opacity(void) { return opacity; }
bool rg_touch_calibrated(void) { return calibrated; }
bool rg_touch_labels(void) { return show_labels; }

void rg_touch_recover(void)
{
    enabled = true;
    input_enabled = true;
    show_labels = true;
    opacity = 70;
    RG_LOGW("BOOT held: touch controls restored for this session; calibrated=%d", calibrated);
}

bool rg_touch_set_labels(bool show)
{
    show_labels = show;
    rg_settings_set_boolean(NS_GLOBAL, "TouchLabels", show);
    bool saved = rg_settings_commit();
    rg_display_force_redraw();
    return saved;
}

bool rg_touch_configure(bool on, int colour, int alpha)
{
    enabled = on;
    colour_index = RG_MIN(RG_MAX(colour, 0), (int)RG_COUNT(colours) - 1);
    opacity = RG_MIN(RG_MAX(alpha, 10), 100);
    rg_settings_set_boolean(NS_GLOBAL, "TouchOverlay", enabled);
    rg_settings_set_number(NS_GLOBAL, "TouchColour", colour_index);
    rg_settings_set_number(NS_GLOBAL, "TouchOpacity", opacity);
    bool saved = rg_settings_commit();
    rg_display_force_redraw();
    return saved;
}

static bool read_adc(uint8_t command, int *result)
{
    spi_transaction_t transaction = {
        .length = 24,
        .flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA,
        .tx_data = {command, 0, 0},
    };
    esp_err_t err = spi_device_transmit(touch_spi, &transaction);
    if (err != ESP_OK)
    {
        RG_LOGE("Touch SPI read failed: %s", esp_err_to_name(err));
        return false;
    }
    *result = ((transaction.rx_data[1] << 8) | transaction.rx_data[2]) >> 3;
    return true;
}

uint32_t rg_touch_read(void)
{
    if (!ready) return 0;
    bool down = gpio_get_level(RG_TOUCH_IRQ) == 0;
    int x = 0, y = 0;
    if (down)
    {
        // Differential conversions finish with PD=00 so PENIRQ is restored.
        down = read_adc(0xD0, &x) && read_adc(0x90, &y) &&
               gpio_get_level(RG_TOUCH_IRQ) == 0 && x > 0 && x < 4095 && y > 0 && y < 4095;
    }
    uint32_t keys = 0;
    if (!down) release_pending = false;
    if (down && input_enabled && calibrated && !calibrating && !release_pending)
    {
        float transform[6];
        portENTER_CRITICAL(&touch_lock);
        memcpy(transform, affine, sizeof(transform));
        portEXIT_CRITICAL(&touch_lock);
        int sx = lroundf(transform[0] * x + transform[1] * y + transform[2]);
        int sy = lroundf(transform[3] * x + transform[4] * y + transform[5]);
        for (size_t i = 0; i < RG_COUNT(buttons); ++i)
        {
            const touch_button_t *b = &buttons[i];
            if (sx >= b->x && sx < b->x + b->w && sy >= b->y && sy < b->y + b->h)
                keys |= b->key;
        }
    }
    portENTER_CRITICAL(&touch_lock);
    raw_x = x;
    raw_y = y;
    pressed = down;
    portEXIT_CRITICAL(&touch_lock);
    return keys;
}

// Five-column glyphs keep labels independent of GUI font and drawing state.
static const uint8_t *glyph(char ch)
{
    static const char codes[] = "^v<>ABXYLR-+MO";
    static const uint8_t data[][5] = {
        {4,2,1,2,4}, {1,2,4,2,1}, {4,10,17,0,0}, {0,0,17,10,4},
        {30,5,5,5,30}, {31,21,21,21,10}, {17,10,4,10,17}, {1,2,28,2,1},
        {31,16,16,16,16}, {31,5,13,21,2}, {4,4,4,4,4}, {4,4,31,4,4},
        {31,2,4,2,31}, {14,17,17,17,14},
    };
    const char *found = strchr(codes, ch);
    return found ? data[found - codes] : data[0];
}

void rg_touch_blend(uint16_t *pixels, int left, int top, int width, int height)
{
    if (!ready || !enabled || calibrating) return;
    uint16_t colour = colours[colour_index];
    for (size_t i = 0; i < RG_COUNT(buttons); ++i)
    {
        const touch_button_t *b = &buttons[i];
        int x0 = RG_MAX(left, b->x), x1 = RG_MIN(left + width, b->x + b->w);
        int y0 = RG_MAX(top, b->y), y1 = RG_MIN(top + height, b->y + b->h);
        const uint8_t *label = glyph(b->glyph);
        for (int y = y0; y < y1; ++y)
            for (int x = x0; x < x1; ++x)
            {
                int gx = (x - b->x - (b->w - 10) / 2) / 2;
                int gy = (y - b->y - (b->h - 10) / 2) / 2;
                bool text = show_labels && x >= b->x + (b->w - 10) / 2 &&
                            y >= b->y + (b->h - 10) / 2 &&
                            gx >= 0 && gx < 5 && gy >= 0 && gy < 5 && (label[gx] & (1 << gy));
                bool edge = x < b->x + 2 || x >= b->x + b->w - 2 ||
                            y < b->y + 2 || y >= b->y + b->h - 2;
                int alpha = edge || text ? opacity : opacity / 4;
                uint16_t *p = &pixels[(y - top) * width + x - left];
                unsigned bg = (*p >> 8) | (*p << 8 & 0xFF00);
                unsigned r = (((bg >> 11) & 31) * (100 - alpha) + ((colour >> 11) & 31) * alpha) / 100;
                unsigned g = (((bg >> 5) & 63) * (100 - alpha) + ((colour >> 5) & 63) * alpha) / 100;
                unsigned bcol = ((bg & 31) * (100 - alpha) + (colour & 31) * alpha) / 100;
                unsigned value = r << 11 | g << 5 | bcol;
                *p = (value >> 8) | (value << 8);
            }
    }
}

void rg_touch_init(void)
{
    gpio_set_direction(RG_TOUCH_IRQ, GPIO_MODE_INPUT);
    spi_device_interface_config_t config = {
        .clock_speed_hz = 1000000,
        .mode = 0,
        .spics_io_num = RG_TOUCH_CS,
        .queue_size = 1,
    };
    esp_err_t err = spi_bus_add_device(RG_SCREEN_HOST, &config, &touch_spi);
    if (err != ESP_OK)
    {
        touch_spi = NULL;
        RG_LOGE("Touch initialization failed: %s", esp_err_to_name(err));
        return;
    }
    enabled = rg_settings_get_boolean(NS_GLOBAL, "TouchOverlay", true);
    input_enabled = rg_settings_get_boolean(NS_GLOBAL, "TouchInput", true);
    show_labels = rg_settings_get_boolean(NS_GLOBAL, "TouchLabels", true);
    colour_index = RG_MIN(RG_MAX((int)rg_settings_get_number(NS_GLOBAL, "TouchColour", 0), 0), 4);
    opacity = RG_MIN(RG_MAX((int)rg_settings_get_number(NS_GLOBAL, "TouchOpacity", 50), 10), 100);
    calibrated = rg_settings_get_boolean(NS_GLOBAL, "TouchCalibrated", false);
    for (int i = 0; i < 6; ++i)
    {
        char key[16];
        snprintf(key, sizeof(key), "TouchCal%d", i);
        affine[i] = rg_settings_get_number(NS_GLOBAL, key, 0);
        if (!isfinite(affine[i])) calibrated = false;
    }
    ready = true;
    RG_LOGI("Touch ready; calibration %s; input=%d overlay=%d labels=%d opacity=%d",
            calibrated ? "loaded" : "required", input_enabled, enabled, show_labels, opacity);
}

void rg_touch_deinit(void)
{
    ready = false;
    if (touch_spi)
    {
        esp_err_t err = spi_bus_remove_device(touch_spi);
        if (err != ESP_OK)
            RG_LOGE("Touch shutdown failed: %s", esp_err_to_name(err));
        else
            touch_spi = NULL;
    }
}

static bool calibration_point(int sx, int sy, float *x, float *y)
{
    rg_display_clear(C_BLACK);
    rg_gui_draw_text(80, 120, 320, "Tap and hold the cross\nBOOT cancels", C_WHITE, C_BLACK, RG_TEXT_MULTILINE | RG_TEXT_ALIGN_CENTER);
    rg_display_clear_rect(sx - 10, sy - 1, 21, 3, C_WHITE);
    rg_display_clear_rect(sx - 1, sy - 10, 3, 21, C_WHITE);
    int count = 0;
    int64_t end = rg_system_timer() + 30000000;
    bool released = false;
    float sum_x = 0, sum_y = 0;
    while (rg_system_timer() < end)
    {
        if (gpio_get_level(GPIO_NUM_0) == 0) return false;
        portENTER_CRITICAL(&touch_lock);
        bool down = pressed;
        int rx = raw_x, ry = raw_y;
        portEXIT_CRITICAL(&touch_lock);
        if (!down) released = true;
        if (released && down)
        {
            sum_x += rx;
            sum_y += ry;
            if (++count == 12)
            {
                *x = sum_x / count;
                *y = sum_y / count;
                return true;
            }
        }
        else count = 0, sum_x = sum_y = 0;
        rg_task_delay(20);
        rg_system_tick(0);
    }
    RG_LOGW("Touch calibration timed out");
    return false;
}

bool rg_touch_calibrate(void)
{
    if (!touch_spi) return false;
    rg_display_sync(true);
    calibrating = true;
    float x[3], y[3], result[6];
    bool ok = calibration_point(40, 40, &x[0], &y[0]) &&
              calibration_point(440, 40, &x[1], &y[1]) &&
              calibration_point(40, 280, &x[2], &y[2]);
    if (ok)
    {
        float ux = x[1] - x[0], uy = y[1] - y[0];
        float vx = x[2] - x[0], vy = y[2] - y[0];
        float det = ux * vy - uy * vx;
        ok = fabsf(det) > 10000;
        if (ok)
        {
            result[0] = 400 * vy / det;
            result[1] = -400 * vx / det;
            result[2] = 40 - result[0] * x[0] - result[1] * y[0];
            result[3] = -240 * uy / det;
            result[4] = 240 * ux / det;
            result[5] = 40 - result[3] * x[0] - result[4] * y[0];
            // Stop coordinate conversion while replacing the transform.
            calibrated = false;
            portENTER_CRITICAL(&touch_lock);
            memcpy(affine, result, sizeof(affine));
            portEXIT_CRITICAL(&touch_lock);
            calibrated = true;
            for (int i = 0; i < 6; ++i)
            {
                char key[16];
                snprintf(key, sizeof(key), "TouchCal%d", i);
                rg_settings_set_number(NS_GLOBAL, key, affine[i]);
            }
            rg_settings_set_boolean(NS_GLOBAL, "TouchCalibrated", true);
            ok = rg_settings_commit();
        }
        else RG_LOGE("Touch calibration points are degenerate");
    }
    rg_task_delay(300);
    release_pending = true;
    calibrating = false;
    rg_display_force_redraw();
    return ok;
}
#endif
