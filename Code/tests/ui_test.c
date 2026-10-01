#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "ui.h"
#include "display.h"
#include "network.h"
#include "tablet_tasks.h"

static bool pending;
static todo_snapshot_t update;
static int completions;
bool tablet_tasks_complete(const todo_item_t *item) { ++completions; return true; }
bool tablet_tasks_take(todo_snapshot_t *out) {
    if (!pending) return false;
    *out = update; pending = false; return true;
}
const char *tablet_network_status(void) { return "MQTT CONNECTED"; }
static char pagination[24];

static int64_t now = 1000000;
static int restores, strokes;
static bool erase_mode;
static char page[24];
static int clears, large_fills, second_row_labels, draws;
int64_t esp_timer_get_time(void) { return now; }
void display_clear(void) { ++clears; ++draws; }
void display_rect(unsigned x, unsigned y, unsigned w, unsigned h, unsigned short c) {
    ++draws;
    if (w * h > 10000) ++large_fills;
}
void display_label(unsigned x, unsigned y, const char *s, unsigned n, unsigned short c) {
    ++draws;
    if (x == 44 && y >= 232 && y < 348) ++second_row_labels;
    if (!strncmp(s, "PAGE ", 5)) snprintf(pagination, sizeof(pagination), "%s", s);
    if (!strcmp(s, "TAP TO OPEN") || !strcmp(s, "YOUR TABLET") || !strcmp(s, "NO TASKS YET") || !strcmp(s, "WAITING FOR LIST"))
        snprintf(page, sizeof(page), "%s", s);
}
void display_note_restore(void) { ++restores; }
void display_toolbar(bool erasing) { erase_mode = erasing; }
void display_stroke(int x0, int y0, int x1, int y1, bool erasing) {
    assert(y0 >= CANVAS_TOP && y1 >= CANVAS_TOP);
    assert(erasing == erase_mode);
    ++strokes;
}
static void release(void) {
    ui_touch(false, 0, 0);
    now += 60000;
    ui_touch(false, 0, 0);
}
static void tap(int x, int y) { release(); ui_touch(true, x, y); }
int main(void) {
    ui_init();
    assert(!strcmp(page, "TAP TO OPEN"));
    tap(100, 260); // Wake; same held pen must not also open Notes.
    assert(!strcmp(page, "YOUR TABLET"));
    ui_touch(true, 100, 260);
    assert(restores == 0);
    tap(100, 170);
    assert(!strcmp(page, "WAITING FOR LIST"));
    update.count = 3;
    strcpy(update.items[0].title, "First task");
    strcpy(update.items[1].title, "Second task");
    strcpy(update.items[2].title, "Third task");
    pending = true; ui_tick();
    assert(!strcmp(pagination, "PAGE 1 / 2"));
    tap(24, 120);
    assert(completions == 1);
    ui_touch(true, 24, 120);
    assert(completions == 1); // Holding the pen cannot repeat the command.
    int before_clear = clears, before_fill = large_fills, before_second = second_row_labels;
    update.items[0].completed = true; pending = true; ui_tick();
    assert(clears == before_clear && large_fills == before_fill);
    assert(second_row_labels == before_second);
    int before_draws = draws;
    pending = true; ui_tick();
    assert(draws == before_draws); // Unchanged snapshots draw nothing.
    tap(220, 380);
    assert(!strcmp(pagination, "PAGE 2 / 2"));
    update.count = 0; pending = true; ui_tick();
    assert(!strcmp(page, "NO TASKS YET"));
    assert(!strcmp(pagination, "PAGE 1 / 1"));
    tap(100, 440); // Home
    tap(100, 260); // Notes
    assert(restores == 1);
    release();
    ui_touch(true, 50, 100);
    ui_touch(true, 60, 110);
    assert(strokes == 2);
    tap(150, 20); // Erase button
    assert(erase_mode);
    ui_touch(true, 70, 120); // Dragging out of a button must not erase.
    assert(strokes == 2);
    tap(70, 120);
    assert(strokes == 3);
    tap(30, 20);
    assert(!erase_mode);
    tap(260, 20); // Home
    tap(100, 260); // Restore note, not clear
    assert(restores == 2);
    release();
    now += 60000001;
    ui_tick();
    assert(!strcmp(page, "TAP TO OPEN"));
    puts("UI navigation, input suppression, tools, restore and idle tests passed");
}
