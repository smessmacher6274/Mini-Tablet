#include "ui.h"
#include "display.h"
#include "esp_timer.h"
#include "network.h"
#include "tablet_tasks.h"
#include <stdio.h>
#include <time.h>
#include <string.h>
#include <stdlib.h>

typedef enum { IDLE, MENU, TODOS, NOTES } screen_t;
static screen_t screen;
static bool erasing, stroke_down, consumed;
static int last_x, last_y;
static int64_t last_input, released_at;
static char clock_text[8];
static const unsigned short INK = 0x2104;
static todo_snapshot_t tasks;
static bool tasks_received;
static unsigned todo_page;
static char last_status[32];
static int64_t message_until;
static todo_item_t visible[2];
static unsigned visible_count, visible_page, visible_total;
static bool visible_received;

static void draw_todo_content(bool force) {
    unsigned pages = (tasks.count + 1) / 2;
    if (!pages) pages = 1;
    if (todo_page >= pages) todo_page = pages - 1;
    bool layout = force || visible_page != todo_page ||
                  (!visible_count != !tasks.count) || visible_received != tasks_received;
    if (force || visible_page != todo_page || visible_total != tasks.count) {
        display_rect(0, 70, 320, 22, 0xffff);
        char label[24];
        snprintf(label, sizeof(label), "PAGE %u / %u", todo_page + 1, pages);
        display_label(22, 72, label, 2, INK);
    }
    if (layout) display_rect(0, 100, 320, 248, 0xffff);
    if (!tasks.count && layout) {
        display_label(22, 180, tasks_received ? "NO TASKS YET" : "WAITING FOR LIST", 3, INK);
        display_label(22, 225, "ADD ON YOUR COMPUTER", 2, INK);
    }
    unsigned count = tasks.count > todo_page * 2 ? tasks.count - todo_page * 2 : 0;
    if (count > 2) count = 2;
    for (unsigned n = 0; n < 2; ++n) {
        unsigned y = 110 + n * 122;
        if (n >= count) {
            if (!layout && n < visible_count) display_rect(0, y, 320, 110, 0xffff);
            continue;
        }
        todo_item_t *task = &tasks.items[todo_page * 2 + n];
        bool same_text = n < visible_count && !strcmp(task->title, visible[n].title);
        if (!layout && same_text && task->completed == visible[n].completed && task->local == visible[n].local) continue;
        // Completion only recolors existing glyphs; title changes clear the row.
        if (!layout && !same_text) display_rect(0, y, 320, 110, 0xffff);
        display_rect(14, y, 20, 20, 0x0470);
        if (!task->completed) display_rect(17, y + 3, 14, 14, 0xffff);
        else display_label(18, y + 3, "X", 2, 0xffff);
        display_rect(14, y + 24, 24, 9, 0xffff);
        if (task->local) display_label(14, y + 24, "BLE", 1, INK);
        const unsigned char *p = (const unsigned char *)task->title;
        for (unsigned line = 0; *p && line < 6; ++line) {
            char text[23]; unsigned used = 0;
            while (*p && used < 22) {
                if (*p < 128) text[used++] = (char)*p++;
                else {
                    text[used++] = '?'; ++p;
                    while ((*p & 0xc0) == 0x80) ++p;
                }
            }
            text[used] = '\0';
            display_label(44, y + line * 16, text, 2, task->completed ? 0x8410 : INK);
        }
    }
    for (unsigned n = 0; n < count; ++n) visible[n] = tasks.items[todo_page * 2 + n];
    visible_count = count; visible_page = todo_page;
    visible_total = tasks.count; visible_received = tasks_received;
}

static void draw_todos(void) {
    display_clear();
    display_label(52, 12, "TO-DO LIST", 3, INK);
    snprintf(last_status, sizeof(last_status), "%s", tablet_network_status());
    display_label(22, 46, last_status, 2, INK);
    draw_todo_content(true);
    display_rect(14, 358, 138, 44, 0x0470);
    display_rect(168, 358, 138, 44, 0x0470);
    display_label(46, 372, "PREV", 3, 0xffff);
    display_label(198, 372, "NEXT", 3, 0xffff);
    display_rect(28, 420, 264, 52, 0x0470);
    display_label(120, 438, "HOME", 3, 0xffff);
}

static void button(int x, int y, int w, const char *label) {
    display_rect(x, y, w, 62, 0x0470);
    display_label(x + 16, y + 22, label, 3, 0xffff);
}

static void update_clock(void) {
    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    char value[8] = "--:--";
    if (local.tm_year >= 124) strftime(value, sizeof(value), "%H:%M", &local);
    if (!strcmp(value, clock_text)) return;
    strcpy(clock_text, value);
    display_rect(0, 170, 320, 120, 0xffff);
    display_label(43, 175, value, 8, INK);
    display_label(52, 260, local.tm_year >= 124 ? "LOCAL TIME" : "TIME NOT SET", 3, INK);
}

static void show(screen_t next) {
    screen = next;
    stroke_down = false;
    consumed = true; // Require a release before interacting with the new screen.
    if (next == NOTES) {
        display_note_restore();
        display_toolbar(erasing);
    } else {
        if (next != TODOS) display_clear();
        switch (next) {
            case IDLE:
                display_label(52, 85, "NOTE TABLET", 3, INK);
                clock_text[0] = '\0';
                update_clock();
                display_label(52, 365, "TAP TO OPEN", 3, INK);
                break;
            case MENU:
                display_label(52, 60, "YOUR TABLET", 3, INK);
                button(28, 145, 264, "TO-DO LIST");
                button(28, 235, 264, "DRAW NOTES");
                button(28, 365, 264, "CLOCK");
                break;
            case TODOS:
                draw_todos();
                break;
            default: break;
        }
    }
    // Redraws at the conservative SPI speed can take seconds.
    last_input = esp_timer_get_time();
}

void ui_init(void) { show(IDLE); }

void ui_touch(bool pressed, int x, int y) {
    int64_t now = esp_timer_get_time();
    if (!pressed) {
        stroke_down = false;
        if (!released_at) released_at = now;
        if (now - released_at >= 50000) consumed = false;
        return;
    }
    released_at = 0;
    last_input = now;
    if (consumed) return;
    if (screen == IDLE) { show(MENU); return; }
    if (screen == MENU) {
        consumed = true;
        if (x >= 28 && x < 292) {
            if (y >= 145 && y < 207) show(TODOS);
            else if (y >= 235 && y < 297) show(NOTES);
            else if (y >= 365 && y < 427) show(IDLE);
        }
        return;
    }
    if (screen == TODOS) {
        consumed = true;
        if (x >= 28 && x < 292 && y >= 420 && y < 472) show(MENU);
        else if (x >= 4 && x < 44) {
            for (unsigned row = 0; row < 2; ++row) {
                unsigned index = todo_page * 2 + row;
                int top = 100 + row * 122;
                if (index < tasks.count && y >= top && y < top + 44) {
                    bool sent = tablet_tasks_complete(&tasks.items[index]);
                    display_rect(0, 44, 320, 20, 0xffff);
                    display_label(22, 46, sent ? (tasks.items[index].local ? "UPDATED LOCALLY" : "SENDING UPDATE") : "CONNECT TO UPDATE", 2, INK);
                    message_until = now + 3000000;
                    break;
                }
            }
        }
        else if (y >= 358 && y < 402) {
            if (x >= 14 && x < 152 && todo_page) { --todo_page; draw_todo_content(false); }
            else if (x >= 168 && x < 306 && (todo_page + 1) * 2 < tasks.count) { ++todo_page; draw_todo_content(false); }
        }
        return;
    }
    if (y < CANVAS_TOP) {
        stroke_down = false;
        consumed = true;
        if (y >= 6 && y < 48) {
            if (x >= 216 && x < 316) { show(MENU); return; }
            if (x >= 4 && x < 104) erasing = false;
            else if (x >= 110 && x < 210) erasing = true;
            display_toolbar(erasing);
        }
        return;
    }
    if (!stroke_down || abs(x - last_x) > 100 || abs(y - last_y) > 100)
        display_stroke(x, y, x, y, erasing);
    else display_stroke(last_x, last_y, x, y, erasing);
    last_x = x; last_y = y; stroke_down = true;
}

void ui_tick(void) {
    if (message_until && esp_timer_get_time() >= message_until) {
        message_until = 0;
        last_status[0] = '\0';
    }
    if (tablet_tasks_take(&tasks)) {
        tasks_received = true;
        if (screen == TODOS) {
            draw_todo_content(false);
            if (message_until) { message_until = 0; last_status[0] = '\0'; }
        }
    }
    if (screen == TODOS && strcmp(last_status, tablet_network_status())) {
        snprintf(last_status, sizeof(last_status), "%s", tablet_network_status());
        display_rect(0, 44, 320, 20, 0xffff);
        display_label(22, 46, last_status, 2, INK);
    }
    if (screen != IDLE && esp_timer_get_time() - last_input >= 60000000)
        show(IDLE);
    if (screen == IDLE) update_clock();
}
