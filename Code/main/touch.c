#include "touch.h"
#include "board.h"
#include "display.h"
#include "ui.h"
#include "test_mode.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdbool.h>
#include <stdlib.h>

static spi_device_handle_t touch;
static const char *TAG = "pen";
typedef struct { int x, y; } point_t;
static point_t origin;
static float ax, bx, ay, by;

static void pause_sample(void) { vTaskDelay(pdMS_TO_TICKS(10) + 1); }

static uint16_t adc(uint8_t cmd) {
    // XPT2046: command followed by 12-bit result, MSB first, shifted by 3.
    // PD1:PD0=00 restores power-down and PENIRQ after each conversion.
    spi_transaction_t t = {
        .flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA,
        .length = 24, .tx_data = {cmd, 0, 0},
    };
    ESP_ERROR_CHECK(spi_device_polling_transmit(touch, &t));
    return (((uint16_t)t.rx_data[1] << 8) | t.rx_data[2]) >> 3 & 0x0fff;
}

static int median(int a[5]) {
    for (int i = 1; i < 5; ++i) {
        int v = a[i], j = i;
        while (j && a[j - 1] > v) { a[j] = a[j - 1]; --j; }
        a[j] = v;
    }
    return a[2];
}

static bool sample(point_t *p) {
    if (gpio_get_level(TOUCH_IRQ)) return false;
    int z1 = adc(0xb0), z2 = adc(0xc0);
    if (z1 < 100 || z2 > 4090 || z1 + 4095 - z2 < 200) return false;
    int x[5], y[5];
    for (int i = 0; i < 5; ++i) {
        (void)adc(0xd0); // Discard first sample after changing axes.
        x[i] = adc(0xd0);
        (void)adc(0x90);
        y[i] = adc(0x90);
    }
    if (gpio_get_level(TOUCH_IRQ)) return false;
    p->x = median(x); p->y = median(y);
    return p->x > 20 && p->x < 4075 && p->y > 20 && p->y < 4075;
}

static void wait_release(void) {
    int released = 0;
    while (released < 5) {
        released = gpio_get_level(TOUCH_IRQ) ? released + 1 : 0;
        pause_sample();
    }
}

static point_t capture(int x, int y) {
    display_clear();
    display_target(x, y);
    ESP_LOGI(TAG, "Hold pen on red cross (%d,%d), then lift", x, y);
    wait_release();
    point_t p = {0}, previous = {0};
    int count = 0, sum_x = 0, sum_y = 0;
    while (count < 12) {
        if (sample(&p)) {
            if (count && (abs(p.x - previous.x) > 60 || abs(p.y - previous.y) > 60)) {
                count = sum_x = sum_y = 0;
            }
            sum_x += p.x; sum_y += p.y; ++count;
            previous = p;
        } else { count = sum_x = sum_y = 0; }
        pause_sample();
    }
    p.x = sum_x / count; p.y = sum_y / count;
    // Erase target as acknowledgment; next cross appears after pen lift.
    display_clear();
    wait_release();
    return p;
}

static void calibrate(void) {
    for (;;) {
        point_t a = capture(30, 30);
        point_t b = capture(290, 30);
        point_t c = capture(30, 450);
        float ux = b.x - a.x, uy = b.y - a.y;
        float vx = c.x - a.x, vy = c.y - a.y;
        float det = ux * vy - uy * vx;
        if (det > -200000 && det < 200000) {
            ESP_LOGW(TAG, "Invalid calibration; repeat three targets");
            continue;
        }
        // Affine transform supports swapped/reversed axes automatically.
        origin = a;
        ax = 260 * vy / det; bx = -260 * vx / det;
        ay = -420 * uy / det; by = 420 * ux / det;
        break;
    }
    display_clear();
    ESP_LOGI(TAG, "Canvas ready. Draw with the pen. Reset to clear/recalibrate.");
}

void touch_drawing_run(void) {
    if (TABLET_BLE_TEST_MODE) {
        ESP_LOGI(TAG, "BLE test mode: calibration and touch disabled");
        ui_init();
        for (;;) {
            ui_tick();
            pause_sample();
        }
    }
    gpio_config_t irq = {
        .pin_bit_mask = 1ULL << TOUCH_IRQ, .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&irq));
    // Independent SPI3 bus: existing LCD wiring stays unchanged.
    spi_bus_config_t bus = {
        .mosi_io_num = TOUCH_DIN, .miso_io_num = TOUCH_DO,
        .sclk_io_num = TOUCH_CLK, .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = 3,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_DISABLED));
    spi_device_interface_config_t dev = {
        .clock_speed_hz = 500000, .mode = 0,
        .spics_io_num = TOUCH_CS, .queue_size = 1,
        .cs_ena_pretrans = 2, .cs_ena_posttrans = 2,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI3_HOST, &dev, &touch));
    (void)adc(0x90); // Ensure PENIRQ is enabled before waiting for a press.
    calibrate();
    ui_init();
    for (;;) {
        point_t p;
        if (sample(&p)) {
            float rx = p.x - origin.x, ry = p.y - origin.y;
            int x = (int)(30 + ax * rx + bx * ry);
            int y = (int)(30 + ay * rx + by * ry);
            if (x < 0) x = 0;
            if (x > 319) x = 319;
            if (y < 0) y = 0;
            if (y > 479) y = 479;
            ui_touch(true, x, y);
        } else ui_touch(false, 0, 0);
        ui_tick();
        pause_sample();
    }
}
