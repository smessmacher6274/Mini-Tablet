#include "display.h"
#include "board.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

enum { CS = TFT_CS, SCK = TFT_SCK, MOSI = TFT_MOSI, RESET = TFT_RST, DC = TFT_DC,
       WIDTH = 320, HEIGHT = 480 };

static spi_device_handle_t display_spi;
// One bit per canvas pixel. Navigation redraws do not change this buffer.
static uint8_t note_bits[(320 * (480 - CANVAS_TOP) + 7) / 8];
static void sleep_ms(unsigned ms) {
    vTaskDelay(pdMS_TO_TICKS(ms) + 1);
}

static void gpio_put(int pin, int level) {
    ESP_ERROR_CHECK(gpio_set_level(pin, level));
}

static void write_bytes(const uint8_t *data, size_t size) {
    spi_transaction_t transaction = {
        .length = size * 8,
        .tx_buffer = data,
    };
    ESP_ERROR_CHECK(spi_device_polling_transmit(display_spi, &transaction));
}

static void command(uint8_t cmd, const uint8_t *data, size_t count) {
    gpio_put(CS, 0);
    gpio_put(DC, 0);
    write_bytes(&cmd, 1);
    if (count) {
        gpio_put(DC, 1);
        write_bytes(data, count);
    }
    gpio_put(CS, 1);
}

#define CMD(c, ...) do { const uint8_t d[] = {__VA_ARGS__}; \
    command((c), d, sizeof(d)); } while (0)

static void fill(unsigned x, unsigned y, unsigned w, unsigned h, uint16_t color) {
    if (x >= WIDTH || y >= HEIGHT || !w || !h) return;
    if (w > WIDTH - x) w = WIDTH - x;
    if (h > HEIGHT - y) h = HEIGHT - y;
    unsigned right = x + w - 1, bottom = y + h - 1;
    CMD(0x2A, x >> 8, x, right >> 8, right);
    CMD(0x2B, y >> 8, y, bottom >> 8, bottom);
    // One scanline keeps RAM available for BLE and needs no external PSRAM.
    uint8_t row[WIDTH * 2];
    for (unsigned i = 0; i < w; ++i) {
        row[2 * i] = color >> 8;
        row[2 * i + 1] = color;
    }
    uint8_t write_memory = 0x2C;
    gpio_put(CS, 0);
    gpio_put(DC, 0);
    write_bytes(&write_memory, 1);
    gpio_put(DC, 1);
    for (unsigned j = 0; j < h; ++j) {
        write_bytes(row, w * 2);
        // Allow the idle task to run during a full-screen fill at 1 MHz.
        if ((j & 15u) == 15u) vTaskDelay(1);
    }
    gpio_put(CS, 1);
}

// Small original 5x7 font: only the characters needed for this demo.
static const char letters[] = "HeloWrd!itEas";
static const uint8_t glyphs[][7] = {
    {17,17,17,31,17,17,17}, // H
    {0,0,14,17,31,16,14},  // e
    {12,4,4,4,4,4,14},     // l
    {0,0,14,17,17,17,14},  // o
    {17,17,17,21,21,21,10},// W
    {0,0,22,25,16,16,16},  // r
    {1,1,15,17,17,17,15},  // d
    {4,4,4,4,4,0,4},       // !
    {4,0,12,4,4,4,14},     // i
    {4,4,14,4,4,4,3},      // t
    {31,16,16,30,16,16,31},// E
    {0,0,14,1,15,17,15},   // a
    {0,0,15,16,14,1,30},   // s
};

static void text(unsigned x, unsigned y, const char *s, unsigned scale) {
    for (; *s; ++s, x += 6 * scale) {
        const char *p = strchr(letters, *s);
        if (!p) continue;
        const uint8_t *g = glyphs[p - letters];
        for (unsigned row = 0; row < 7; ++row)
            for (unsigned col = 0; col < 5; ++col)
                if (g[row] & (1u << (4 - col)))
                    fill(x + col * scale, y + row * scale, scale, scale, 0xffff);
    }
}

void display_clear(void) { fill(0, 0, WIDTH, HEIGHT, 0xffff); }

void display_rect(unsigned x, unsigned y, unsigned w, unsigned h, unsigned short color) {
    fill(x, y, w, h, color);
}

// Original 5x7 uppercase UI font, A-Z followed by 0-9 and punctuation.
static const char ui_letters[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789:-/.?";
static const uint8_t ui_font[][7] = {
    {14,17,17,31,17,17,17}, {30,17,17,30,17,17,30},
    {14,17,16,16,16,17,14}, {30,17,17,17,17,17,30},
    {31,16,16,30,16,16,31}, {31,16,16,30,16,16,16},
    {14,17,16,23,17,17,15}, {17,17,17,31,17,17,17},
    {14,4,4,4,4,4,14}, {7,2,2,2,18,18,12},
    {17,18,20,24,20,18,17}, {16,16,16,16,16,16,31},
    {17,27,21,21,17,17,17}, {17,25,25,21,19,19,17},
    {14,17,17,17,17,17,14}, {30,17,17,30,16,16,16},
    {14,17,17,17,21,18,13}, {30,17,17,30,20,18,17},
    {15,16,16,14,1,1,30}, {31,4,4,4,4,4,4},
    {17,17,17,17,17,17,14}, {17,17,17,17,17,10,4},
    {17,17,17,21,21,21,10}, {17,17,10,4,10,17,17},
    {17,17,10,4,4,4,4}, {31,1,2,4,8,16,31},
    {14,17,19,21,25,17,14}, {4,12,4,4,4,4,14},
    {14,17,1,2,4,8,31}, {30,1,1,14,1,1,30},
    {2,6,10,18,31,2,2}, {31,16,16,30,1,1,30},
    {14,16,16,30,17,17,14}, {31,1,2,4,8,8,8},
    {14,17,17,14,17,17,14}, {14,17,17,15,1,1,14},
    {0,4,4,0,4,4,0}, {0,0,0,31,0,0,0},
    {1,2,2,4,8,8,16}, {0,0,0,0,0,4,4},
    {14,17,1,2,4,0,4},
};

void display_label(unsigned x, unsigned y, const char *s, unsigned scale,
                   unsigned short color) {
    for (; *s && x + 5 * scale <= WIDTH; ++s, x += 6 * scale) {
        if (*s == ' ') continue;
        const char *p = strchr(ui_letters, toupper((unsigned char)*s));
        if (!p) p = strchr(ui_letters, '?');
        const uint8_t *g = ui_font[p - ui_letters];
        for (unsigned row = 0; row < 7; ++row)
            for (unsigned col = 0; col < 5; ++col)
                if (g[row] & (1u << (4 - col)))
                    fill(x + col * scale, y + row * scale, scale, scale, color);
    }
}

void display_note_restore(void) {
    uint8_t row[WIDTH * 2];
    CMD(0x2A, 0, 0, (WIDTH - 1) >> 8, (WIDTH - 1) & 255);
    CMD(0x2B, 0, CANVAS_TOP, (HEIGHT - 1) >> 8, (HEIGHT - 1) & 255);
    uint8_t cmd = 0x2c;
    gpio_put(CS, 0); gpio_put(DC, 0); write_bytes(&cmd, 1); gpio_put(DC, 1);
    for (unsigned y = 0; y < HEIGHT - CANVAS_TOP; ++y) {
        for (unsigned x = 0; x < WIDTH; ++x) {
            unsigned bit = y * WIDTH + x;
            uint8_t c = (note_bits[bit / 8] & (1u << (bit % 8))) ? 0 : 255;
            row[2 * x] = row[2 * x + 1] = c;
        }
        write_bytes(row, sizeof(row));
        if ((y & 15u) == 15u) vTaskDelay(1);
    }
    gpio_put(CS, 1);
}

void display_target(int x, int y) {
    fill(x - 12, y - 1, 25, 3, 0xf800);
    fill(x - 1, y - 12, 3, 25, 0xf800);
}

void display_toolbar(bool erasing) {
    fill(0, 0, WIDTH, CANVAS_TOP, 0x2104);
    fill(4, 6, 100, 42, erasing ? 0x4208 : 0x0470);
    fill(110, 6, 100, 42, erasing ? 0x0470 : 0x4208);
    fill(216, 6, 100, 42, 0x4208);
    display_label(24, 20, "WRITE", 2, 0xffff);
    display_label(130, 20, "ERASE", 2, 0xffff);
    display_label(242, 20, "HOME", 2, 0xffff);
}

void display_stroke(int x0, int y0, int x1, int y1, bool erasing) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int error = dx + dy;
    for (;;) {
        // Clip the brush to the canvas, including its radius at the toolbar.
        int radius = erasing ? 8 : 1;
        int left = x0 - radius, top = y0 - radius;
        int right = x0 + radius, bottom = y0 + radius;
        if (left < 0) left = 0;
        if (top < CANVAS_TOP) top = CANVAS_TOP;
        if (right >= WIDTH) right = WIDTH - 1;
        if (bottom >= HEIGHT) bottom = HEIGHT - 1;
        if (right >= left && bottom >= top) {
            for (int y = top; y <= bottom; ++y)
                for (int x = left; x <= right; ++x) {
                    unsigned bit = (y - CANVAS_TOP) * WIDTH + x;
                    if (erasing) note_bits[bit / 8] &= ~(1u << (bit % 8));
                    else note_bits[bit / 8] |= 1u << (bit % 8);
                }
            fill(left, top, right - left + 1, bottom - top + 1,
                 erasing ? 0xffff : 0x0000);
        }
        if (x0 == x1 && y0 == y1) break;
        int twice = 2 * error;
        if (twice >= dy) { error += dy; x0 += sx; }
        if (twice <= dx) { error += dx; y0 += sy; }
    }
}

void display_hello_world(void) {
    // Conservative clock for breadboard jumper wires; SPI mode 0, MSB first.
    const int pins[] = {CS, DC, RESET};
    for (unsigned i = 0; i < 3; ++i) {
        ESP_ERROR_CHECK(gpio_reset_pin(pins[i]));
        gpio_put(pins[i], 1);
        ESP_ERROR_CHECK(gpio_set_direction(pins[i], GPIO_MODE_OUTPUT));
    }
    spi_bus_config_t bus = {
        .mosi_io_num = MOSI, .miso_io_num = -1, .sclk_io_num = SCK,
        .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = WIDTH * 2,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));
    spi_device_interface_config_t device = {
        .clock_speed_hz = TFT_SPI_HZ, .mode = 0,
        .spics_io_num = -1, .queue_size = 1,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &device, &display_spi));
    sleep_ms(20);
    gpio_put(RESET, 0);
    sleep_ms(20);
    gpio_put(RESET, 1);
    sleep_ms(150);
    command(0x01, NULL, 0); // Software reset
    sleep_ms(150);
    command(0x11, NULL, 0); // Sleep out
    sleep_ms(120);
    // ST7796S register setup, following the controller sequence documented in
    // https://github.com/Bodmer/TFT_eSPI/blob/master/TFT_Drivers/ST7796_Init.h
    CMD(0xF0, 0xC3);
    CMD(0xF0, 0x96);
    CMD(0x36, 0x48); // Portrait: MX + BGR (320 x 480)
    CMD(0x3A, 0x55); // RGB565
    CMD(0xB4, 0x01);
    CMD(0xB6, 0x80, 0x02, 0x3B);
    CMD(0xE8, 0x40, 0x8A, 0x00, 0x00, 0x29, 0x19, 0xA5, 0x33);
    CMD(0xC1, 0x06);
    CMD(0xC2, 0xA7);
    CMD(0xC5, 0x18);
    sleep_ms(120);
    CMD(0xE0, 0xF0,0x09,0x0B,0x06,0x04,0x15,0x2F,0x54,0x42,0x3C,0x17,0x14,0x18,0x1B);
    CMD(0xE1, 0xE0,0x09,0x0B,0x06,0x04,0x03,0x2B,0x43,0x42,0x3B,0x16,0x14,0x17,0x1B);
    sleep_ms(120);
    CMD(0xF0, 0x3C);
    CMD(0xF0, 0x69);
    command(0x20, NULL, 0); // Inversion off
    fill(0, 0, WIDTH, HEIGHT, 0x0843);
    text(73, 170, "Hello", 6);
    text(55, 235, "World!", 6);
    // RGB bars help check pixel format and color order.
    fill(30, 330, 80, 12, 0xf800);
    fill(120, 330, 80, 12, 0x07e0);
    fill(210, 330, 80, 12, 0x001f);
    command(0x29, NULL, 0); // Display on
    sleep_ms(20);
}
