#include "touch.h"
#include "touch_calibration.h"
#include "board.h"
#include "display.h"
#include "ui.h"
#include "test_mode.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

static spi_device_handle_t touch;
static const char *TAG = "pen";
typedef struct { int x, y; } point_t;
static point_t origin;
static float ax, bx, ay, by;
static bool hardware_initialized;
static bool nvs_ready;

static void apply_calibration(const touch_calibration_blob_t *c) {
    origin = (point_t){c->origin_x, c->origin_y};
    ax = c->ax; bx = c->bx; ay = c->ay; by = c->by;
}

static bool load_calibration(void) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open("touch", NVS_READONLY, &handle);
    if (err != ESP_OK) return false;
    touch_calibration_blob_t c;
    size_t len = sizeof(c);
    err = nvs_get_blob(handle, "cal_v1", &c, &len);
    nvs_close(handle);
    if (err != ESP_OK || len != sizeof(c) || !touch_calibration_valid(&c)) {
        ESP_LOGW(TAG, "Saved calibration missing or invalid; calibrating manually");
        return false;
    }
    apply_calibration(&c);
    ESP_LOGI(TAG, "Loaded saved touch calibration");
    return true;
}

static void save_calibration(void) {
    if (!nvs_ready) return;
    touch_calibration_blob_t c = {
        .magic = TOUCH_CAL_MAGIC, .version = TOUCH_CAL_VERSION, .size = sizeof(c),
        .origin_x = origin.x, .origin_y = origin.y,
        .ax = ax, .bx = bx, .ay = ay, .by = by,
    };
    if (!touch_calibration_valid(&c)) {
        ESP_LOGE(TAG, "Refusing to save invalid touch calibration");
        return;
    }
    nvs_handle_t handle;
    esp_err_t err = nvs_open("touch", NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_blob(handle, "cal_v1", &c, sizeof(c));
        if (err == ESP_OK) err = nvs_commit(handle);
        nvs_close(handle);
    }
    if (err != ESP_OK) ESP_LOGE(TAG, "Could not save touch calibration: %s", esp_err_to_name(err));
    else ESP_LOGI(TAG, "Saved touch calibration");
}

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
        touch_calibration_blob_t candidate = {
            .magic = TOUCH_CAL_MAGIC, .version = TOUCH_CAL_VERSION, .size = sizeof(candidate),
            .origin_x = origin.x, .origin_y = origin.y,
            .ax = ax, .bx = bx, .ay = ay, .by = by,
        };
        if (!touch_calibration_valid(&candidate)) {
            ESP_LOGW(TAG, "Calibration coefficients unreasonable; repeat three targets");
            continue;
        }
        break;
    }
    display_clear();
    ESP_LOGI(TAG, "Touch calibrated. Use the Home screen Recalibrate button to recalibrate.");
}

void touch_init_calibrated(void) {
    if (hardware_initialized) return;
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
    hardware_initialized = true;
    (void)adc(0x90); // Ensure PENIRQ is enabled before waiting for a press.
    // BLE may initialize NVS concurrently. Do not erase it on any error; fall back
    // to interactive calibration and leave persistence disabled for this boot.
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_OK) nvs_ready = true;
    else ESP_LOGW(TAG, "NVS unavailable (%s); using manual calibration", esp_err_to_name(nvs_err));
    if (!nvs_ready || !load_calibration()) {
        calibrate();
        save_calibration();
    }
}

void touch_recalibrate(void) {
    if (!hardware_initialized) {
        touch_init_calibrated();
        return;
    }
    calibrate();
    save_calibration();
}

bool touch_is_down(void) { return gpio_get_level(TOUCH_IRQ) == 0; }

bool touch_read(int *x, int *y) {
    point_t p;
    if (!sample(&p)) return false;
    float rx = p.x - origin.x, ry = p.y - origin.y;
    *x = (int)(30 + ax * rx + bx * ry);
    *y = (int)(30 + ay * rx + by * ry);
    if (*x < 0) *x = 0;
    if (*x > 319) *x = 319;
    if (*y < 0) *y = 0;
    if (*y > 479) *y = 479;
    return true;
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
    touch_init_calibrated();
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
