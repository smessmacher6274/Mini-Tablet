#include <assert.h>
#include <math.h>
#include <stdio.h>

#include "touch_calibration.h"

static touch_calibration_blob_t valid_normal(void) {
    return (touch_calibration_blob_t){
        .magic = TOUCH_CAL_MAGIC,
        .version = TOUCH_CAL_VERSION,
        .size = sizeof(touch_calibration_blob_t),
        .origin_x = 1900,
        .origin_y = 2100,
        .ax = 0.12f, .bx = 0.0f,
        .ay = 0.0f, .by = 0.10f,
    };
}

int main(void) {
    touch_calibration_blob_t c = valid_normal();
    assert(touch_calibration_valid(&c));

    // Swapped axes have a negative determinant and remain valid.
    c.ax = 0.0f; c.bx = 0.10f;
    c.ay = 0.12f; c.by = 0.0f;
    assert(touch_calibration_valid(&c));

    c = valid_normal(); c.version++;
    assert(!touch_calibration_valid(&c));
    c = valid_normal(); c.size--;
    assert(!touch_calibration_valid(&c));
    c = valid_normal(); c.ax = NAN;
    assert(!touch_calibration_valid(&c));
    c = valid_normal(); c.by = INFINITY;
    assert(!touch_calibration_valid(&c));
    c = valid_normal(); c.by = 0.0f;
    assert(!touch_calibration_valid(&c)); // Singular transform.
    c = valid_normal(); c.origin_x = 4090;
    assert(!touch_calibration_valid(&c));
    c = valid_normal(); c.bx = 5.1f;
    assert(!touch_calibration_valid(&c));

    puts("Touch calibration validation passed");
    return 0;
}
