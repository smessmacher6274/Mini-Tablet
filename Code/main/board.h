#pragma once

#include "sdkconfig.h"
// GPIO numbers, NOT physical header positions.
// Confirm these pins are exposed and unused on your exact board.
#if CONFIG_IDF_TARGET_ESP32
#define TFT_CS   27
#define TFT_MOSI 23
#define TFT_SCK  18
#define TFT_DC   26
#define TFT_RST  25
#define TOUCH_CLK 14
#define TOUCH_DIN 13
#define TOUCH_DO  19
#define TOUCH_CS  32
#define TOUCH_IRQ 33
#elif CONFIG_IDF_TARGET_ESP32S3
#define TFT_CS   10
#define TFT_MOSI 11
#define TFT_SCK  12
#define TFT_DC   9
#define TFT_RST  8
#define TOUCH_CLK 14
#define TOUCH_DIN 13
#define TOUCH_DO  15
#define TOUCH_CS  16
#define TOUCH_IRQ 17
#else
#error "Only ESP32 and ESP32-S3 pin maps are defined"
#endif
#define TFT_SPI_HZ 1000000
