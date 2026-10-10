#ifndef DISPLAY_H
#define DISPLAY_H
#include <stdbool.h>
#define CANVAS_TOP 56

// Initialize ESP-IDF SPI2/ST7796S and draw once, before BLE starts.
void display_hello_world(void);
void display_init(void);
void display_clear(void);
void display_target(int x, int y);
void display_toolbar(bool erasing);
void display_stroke(int x0, int y0, int x1, int y1, bool erasing);
void display_rect(unsigned x, unsigned y, unsigned w, unsigned h, unsigned short color);
void display_label(unsigned x, unsigned y, const char *s, unsigned scale, unsigned short color);
void display_note_restore(void);

#endif
