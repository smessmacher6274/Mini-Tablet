#pragma once

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#define TOUCH_CAL_MAGIC 0x5443414cU
#define TOUCH_CAL_VERSION 1U

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    int32_t origin_x, origin_y;
    float ax, bx, ay, by;
} touch_calibration_blob_t;

static inline bool touch_calibration_valid(const touch_calibration_blob_t *c) {
    if (c->magic != TOUCH_CAL_MAGIC || c->version != TOUCH_CAL_VERSION || c->size != sizeof(*c)) return false;
    if (c->origin_x < 20 || c->origin_x > 4075 || c->origin_y < 20 || c->origin_y > 4075) return false;
    if (!isfinite(c->ax) || !isfinite(c->bx) || !isfinite(c->ay) || !isfinite(c->by)) return false;
    // The measured panel covers roughly 260x420 pixels over a few thousand ADC counts.
    if (fabsf(c->ax) > 5 || fabsf(c->bx) > 5 || fabsf(c->ay) > 5 || fabsf(c->by) > 5) return false;
    float det = c->ax * c->by - c->bx * c->ay;
    return isfinite(det) && fabsf(det) > 1.0e-6f && fabsf(det) < 25.0f;
}
