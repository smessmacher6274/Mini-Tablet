#pragma once
#include <stdbool.h>
void touch_init_calibrated(void);
// Re-run calibration and persist it. Safe to call after touch initialization.
void touch_recalibrate(void);
bool touch_read(int *x, int *y);
bool touch_is_down(void);
// Load saved calibration (or calibrate if missing), then run the pen canvas.
void touch_drawing_run(void);
