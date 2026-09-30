#include "display.h"
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include <string.h>

enum { CS = 17, SCK = 18, MOSI = 19, RESET = 20, DC = 21,
       WIDTH = 320, HEIGHT = 480 };

static void command(uint8_t cmd, const uint8_t *data, size_t count) {
    gpio_put(CS, 0);
    gpio_put(DC, 0);
    spi_write_blocking(spi0, &cmd, 1);
    if (count) {
        gpio_put(DC, 1);
        spi_write_blocking(spi0, data, count);
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
    // One scanline, not a 307,200-byte framebuffer (too large for RP2040 RAM).
    uint8_t row[WIDTH * 2];
    for (unsigned i = 0; i < w; ++i) {
        row[2 * i] = color >> 8;
        row[2 * i + 1] = color;
    }
    uint8_t write_memory = 0x2C;
    gpio_put(CS, 0);
    gpio_put(DC, 0);
    spi_write_blocking(spi0, &write_memory, 1);
    gpio_put(DC, 1);
    for (unsigned j = 0; j < h; ++j) spi_write_blocking(spi0, row, w * 2);
    gpio_put(CS, 1);
}

// Small original 5x7 font: only the characters needed for this demo.
static const char letters[] = "HeloWrd!";
static const uint8_t glyphs[][7] = {
    {17,17,17,31,17,17,17}, // H
    {0,0,14,17,31,16,14},  // e
    {12,4,4,4,4,4,14},     // l
    {0,0,14,17,17,17,14},  // o
    {17,17,17,21,21,21,10},// W
    {0,0,22,25,16,16,16},  // r
    {1,1,15,17,17,17,15},  // d
    {4,4,4,4,4,0,4},       // !
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

void display_hello_world(void) {
    // Conservative clock for breadboard jumper wires; SPI mode 0, MSB first.
    spi_init(spi0, 1000000);
    spi_set_format(spi0, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    gpio_set_function(SCK, GPIO_FUNC_SPI);
    gpio_set_function(MOSI, GPIO_FUNC_SPI);
    const uint pins[] = {CS, DC, RESET};
    for (unsigned i = 0; i < 3; ++i) {
        gpio_init(pins[i]);
        gpio_put(pins[i], 1);
        gpio_set_dir(pins[i], GPIO_OUT);
    }
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
