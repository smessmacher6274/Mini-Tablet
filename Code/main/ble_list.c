#include "ble_list.h"
#include "display.h"
#include "touch.h"
#include "checkbox_tap.h"
#include "tablet_tasks.h"
#include "network.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

enum { MAX_BYTES = 120, COLS = 22, TOP = 54, BOTTOM = 438,
       LINE_HEIGHT = 18, GAP = 10, PREV = 1001, NEXT = 1002,
       HOME = 1003, SHOW_LIST = 1004, RECALIBRATE = 1005,
       MAX_RENDERED_ROWS = (BOTTOM - TOP) / (LINE_HEIGHT + GAP) + 1 };
typedef enum { SCREEN_HOME, SCREEN_LIST } screen_t;
typedef struct {
    char id[37];
    char title[TODO_MAX_TITLE + 1];
    unsigned y, height;
    bool completed, valid;
} rendered_row_t;
static todo_snapshot_t tasks;
static todo_snapshot_t incoming;
static unsigned starts[TODO_MAX_VISIBLE], pages = 1, page, first, visible_count;
static rendered_row_t rendered[MAX_RENDERED_ROWS];
static screen_t screen = SCREEN_HOME;
static unsigned footer_page, footer_pages;
static bool rendered_placeholder;
static char last_status[32] = "STARTING";

bool ble_list_valid(const char *text, size_t length) {
    if (!text || !length || length > MAX_BYTES) return false;
    bool content = false;
    for (size_t i = 0; i < length;) {
        unsigned c = (unsigned char)text[i++], extra = 0, minimum = 0;
        if (c < 128) {
            if (c < 32 || c == 127) return false;
            if (c != ' ') content = true;
            continue;
        }
        if (c >= 0xc2 && c <= 0xdf) { c &= 31; extra = 1; minimum = 0x80; }
        else if (c >= 0xe0 && c <= 0xef) { c &= 15; extra = 2; minimum = 0x800; }
        else if (c >= 0xf0 && c <= 0xf4) { c &= 7; extra = 3; minimum = 0x10000; }
        else return false;
        if (i + extra > length) return false;
        while (extra--) {
            unsigned next = (unsigned char)text[i++];
            if ((next & 0xc0) != 0x80) return false;
            c = (c << 6) | (next & 63);
        }
        if (c < minimum || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff) ||
            (c >= 0x80 && c <= 0x9f) || c == 0x2028 || c == 0x2029) return false;
        content = true;
    }
    return content;
}

bool ble_list_submit(const char *text, size_t length, unsigned number) {
    (void)number;
    return ble_list_valid(text, length) && tablet_tasks_add(text, length) == TASK_ADD_OK;
}

static void screen_text(const char *text, char output[MAX_BYTES + 1]) {
    size_t out = 0;
    for (size_t i = 0; text[i] && out < MAX_BYTES; ++i) {
        unsigned char c = text[i];
        if (c < 128) output[out++] = c;
        else if ((c & 0xc0) != 0x80) output[out++] = '?';
    }
    output[out] = 0;
}

// Word wrapping and height calculation share exactly the same layout.
static unsigned layout(const char *text, unsigned y, bool draw, bool completed) {
    unsigned lines = 0;
    while (*text) {
        while (*text == ' ') ++text;
        if (!*text) break;
        size_t take = strlen(text);
        if (take > COLS) {
            take = COLS;
            size_t split = take;
            while (split && text[split] != ' ') --split;
            if (split) take = split;
        }
        char line[COLS + 1];
        memcpy(line, text, take);
        line[take] = 0;
        if (draw) {
            display_label(40, y + lines * LINE_HEIGHT, line, 2, completed ? 0x8410 : 0x0000);
            if (completed) display_rect(40, y + lines * LINE_HEIGHT + 7,
                                        take * 12 - 2, 1, 0x8410);
        }
        ++lines;
        text += take;
    }
    return lines * LINE_HEIGHT + GAP;
}

static unsigned item_height(const todo_item_t *item) {
    char text[MAX_BYTES + 1];
    screen_text(item->title, text);
    return layout(text, 0, false, false);
}

static void draw_item(const todo_item_t *item, unsigned y) {
    display_rect(12, y, 18, 18, 0x4208);
    display_rect(14, y + 2, 14, 14, 0xffff);
    if (item->completed) display_label(15, y + 2, "X", 2, 0x0470);
    char text[MAX_BYTES + 1];
    screen_text(item->title, text);
    layout(text, y, true, item->completed);
}

static unsigned hit_item(int x, int y) {
    if (screen == SCREEN_HOME) {
        if (x >= 50 && x < 270 && y >= 178 && y < 252) return SHOW_LIST;
        if (x >= 50 && x < 270 && y >= 278 && y < 352) return RECALIBRATE;
        return 0;
    }
    if (x >= 246 && x < 316 && y >= 2 && y < 28) return HOME;
    if (y >= 446 && y < 478) {
        if (x >= 10 && x < 110 && page) return PREV;
        if (x >= 210 && x < 310 && page + 1 < pages) return NEXT;
    }
    if (x < 6 || x >= 36 || y < TOP) return 0;
    unsigned top = TOP;
    for (unsigned i = first; i < first + visible_count; ++i) {
        if ((unsigned)y >= top && (unsigned)y < top + 24) return i + 1;
        top += item_height(&tasks.items[i]);
    }
    return 0;
}

static void header(const char *status) {
    char label[32];
    if (screen == SCREEN_HOME) snprintf(label, sizeof(label), "HOME");
    else snprintf(label, sizeof(label), "TO DO / %u", tasks.count);
    display_rect(0, 0, 320, TOP, 0xffff);
    display_label(10, 8, label, 2, 0x4208);
    if (screen == SCREEN_LIST) {
        display_rect(244, 2, 72, 26, 0x0470);
        display_label(256, 8, "HOME", 1, 0xffff);
    }
    display_label(10, 30, status, 1, 0x4208);
    display_rect(10, 46, 300, 1, 0xc618);
}

static void update_header_count(void) {
    if (screen != SCREEN_LIST) return;
    char label[24];
    snprintf(label, sizeof(label), "TO DO / %u", tasks.count);
    display_rect(0, 2, 170, 26, 0xffff);
    display_label(10, 8, label, 2, 0x4208);
}

static void update_status(const char *status) {
    display_rect(0, 27, 320, 18, 0xffff);
    display_label(10, 30, status, 1, 0x4208);
}

static void draw_home(void) {
    screen = SCREEN_HOME;
    memset(rendered, 0, sizeof(rendered));
    rendered_placeholder = false;
    header(last_status);
    display_rect(0, TOP, 320, 480 - TOP, 0xffff);
    display_rect(50, 178, 220, 74, 0x0470);
    display_label(88, 194, "TO DO LIST", 2, 0xffff);
    char count[28];
    snprintf(count, sizeof(count), "%u TASKS", tasks.count);
    display_label(118, 226, count, 1, 0xffff);
    display_rect(50, 278, 220, 74, 0x4208);
    display_label(94, 306, "RECALIBRATE", 1, 0xffff);
}

static void paginate(void) {
    pages = 1; starts[0] = 0;
    unsigned height = 0;
    for (unsigned i = 0; i < tasks.count; ++i) {
        unsigned h = item_height(&tasks.items[i]);
        if (height && height + h > BOTTOM - TOP) { starts[pages++] = i; height = 0; }
        height += h;
    }
    if (page >= pages) page = pages - 1;
}

static void draw_page(void) {
    screen = SCREEN_LIST;
    header(last_status);
    display_rect(0, TOP, 320, 480 - TOP, 0xffff);
    memset(rendered, 0, sizeof(rendered));
    rendered_placeholder = tasks.count == 0;
    first = starts[page];
    unsigned end = page + 1 < pages ? starts[page + 1] : tasks.count;
    visible_count = end - first;
    unsigned y = TOP;
    for (unsigned i = first; i < end; ++i) {
        draw_item(&tasks.items[i], y);
        unsigned height = item_height(&tasks.items[i]);
        unsigned slot = i - first;
        if (slot < MAX_RENDERED_ROWS) rendered[slot] = (rendered_row_t){ .y = y, .height = height,
            .completed = tasks.items[i].completed, .valid = true };
        if (slot < MAX_RENDERED_ROWS) {
            snprintf(rendered[slot].id, sizeof(rendered[slot].id), "%s", tasks.items[i].id);
            snprintf(rendered[slot].title, sizeof(rendered[slot].title), "%s", tasks.items[i].title);
        }
        y += height;
    }
    if (!tasks.count) display_label(12, TOP, "NO TASKS YET", 2, 0x8410);
    display_rect(10, 446, 100, 30, page ? 0x0470 : 0xc618);
    display_label(34, 454, "PREV", 2, 0xffff);
    display_rect(210, 446, 100, 30, page + 1 < pages ? 0x0470 : 0xc618);
    display_label(234, 454, "NEXT", 2, 0xffff);
    char label[20];
    snprintf(label, sizeof(label), "%u/%u", page + 1, pages);
    display_label(128, 454, label, 2, 0x4208);
    footer_page = page + 1;
    footer_pages = pages;
}

// Completion updates may change only the checkbox and title styling. Keep the
// page geometry when every item's identity, order, and title still match.
static bool same_layout(const todo_snapshot_t *a, const todo_snapshot_t *b) {
    if (a->count != b->count) return false;
    for (unsigned i = 0; i < a->count; ++i) {
        if (strcmp(a->items[i].id, b->items[i].id) ||
            strcmp(a->items[i].title, b->items[i].title))
            return false;
    }
    return true;
}

static bool same_row_geometry(const rendered_row_t *row, const todo_item_t *item,
                              unsigned y, unsigned height) {
    return row->valid && row->y == y && row->height == height &&
           !strcmp(row->id, item->id) && !strcmp(row->title, item->title);
}

// Preserve the unchanged visible prefix. Once identity or geometry differs,
// erase the affected suffix and paint only the rows that now intersect it.
static void update_visible_rows(void) {
    unsigned new_first = starts[page];
    unsigned new_end = page + 1 < pages ? starts[page + 1] : tasks.count;
    unsigned y = TOP, dirty_y = 480, old_end_y = TOP;
    bool old_placeholder = rendered_placeholder;
    for (unsigned i = new_first; i < new_end; ++i) {
        unsigned slot = i - new_first;
        unsigned height = item_height(&tasks.items[i]);
        if (dirty_y == 480 && !same_row_geometry(&rendered[slot], &tasks.items[i], y, height))
            dirty_y = y;
        y += height;
    }
    unsigned new_end_y = y;
    unsigned old_count = visible_count;
    for (unsigned slot = 0; slot < old_count && slot < MAX_RENDERED_ROWS; ++slot)
        if (rendered[slot].valid && rendered[slot].y + rendered[slot].height > old_end_y)
            old_end_y = rendered[slot].y + rendered[slot].height;
    if (old_count > new_end - new_first && dirty_y == 480 &&
        rendered[new_end - new_first].valid)
        dirty_y = rendered[new_end - new_first].y;
    bool new_placeholder = new_first == new_end;
    if (old_placeholder != new_placeholder && dirty_y == 480) dirty_y = TOP;
    if (dirty_y != 480) {
        unsigned clear_end = old_end_y > new_end_y ? old_end_y : new_end_y;
        if ((old_placeholder || new_placeholder) && clear_end < TOP + 20) clear_end = TOP + 20;
        if (clear_end > BOTTOM) clear_end = BOTTOM;
        if (clear_end > dirty_y) display_rect(0, dirty_y, 320, clear_end - dirty_y, 0xffff);
    }
    y = TOP;
    for (unsigned i = new_first; i < new_end; ++i) {
        unsigned slot = i - new_first;
        unsigned height = item_height(&tasks.items[i]);
        bool geometry_same = same_row_geometry(&rendered[slot], &tasks.items[i], y, height);
        if (dirty_y != 480 && y >= dirty_y) draw_item(&tasks.items[i], y);
        else if (geometry_same && rendered[slot].completed != tasks.items[i].completed) {
            display_rect(0, y, 320, height, 0xffff);
            draw_item(&tasks.items[i], y);
        }
        rendered[slot] = (rendered_row_t){ .y = y, .height = height,
            .completed = tasks.items[i].completed, .valid = true };
        snprintf(rendered[slot].id, sizeof(rendered[slot].id), "%s", tasks.items[i].id);
        snprintf(rendered[slot].title, sizeof(rendered[slot].title), "%s", tasks.items[i].title);
        y += height;
    }
    if (new_placeholder) {
        if (dirty_y != 480) display_label(12, TOP, "NO TASKS YET", 2, 0x8410);
        rendered_placeholder = true;
    } else rendered_placeholder = false;
    for (unsigned slot = new_end - new_first; slot < MAX_RENDERED_ROWS; ++slot)
        rendered[slot].valid = false;
    first = new_first;
    visible_count = new_end - new_first;
    // Page controls are isolated in their own band and only need refresh if pagination changed.
    if (footer_page != page + 1 || footer_pages != pages) {
        display_rect(10, 446, 100, 30, page ? 0x0470 : 0xc618);
        display_label(34, 454, "PREV", 2, 0xffff);
        display_rect(210, 446, 100, 30, page + 1 < pages ? 0x0470 : 0xc618);
        display_label(234, 454, "NEXT", 2, 0xffff);
        char label[20];
        snprintf(label, sizeof(label), "%u/%u", page + 1, pages);
        display_rect(120, 446, 80, 30, 0xffff);
        display_label(128, 454, label, 2, 0x4208);
        footer_page = page + 1;
        footer_pages = pages;
    }
}

static void accept_snapshot(const todo_snapshot_t *next) {
    bool layout_same = same_layout(&tasks, next);
    bool count_changed = tasks.count != next->count;
    tasks = *next;
    if (screen == SCREEN_HOME) {
        if (count_changed) {
            char count[28];
            display_rect(105, 224, 110, 20, 0x0470);
            snprintf(count, sizeof(count), "%u TASKS", tasks.count);
            display_label(118, 226, count, 1, 0xffff);
        }
    } else {
        if (count_changed) update_header_count();
        if (!layout_same) paginate();
        update_visible_rows();
    }
}

static void handle_target(unsigned target, TickType_t *message_until) {
    if (target == HOME && screen == SCREEN_LIST) {
        draw_home();
    } else if (target == SHOW_LIST && screen == SCREEN_HOME) {
        paginate();
        draw_page();
    } else if (target == RECALIBRATE && screen == SCREEN_HOME) {
        touch_recalibrate();
        draw_home();
    } else if (target == PREV || target == NEXT) {
        if (target == PREV && page) --page;
        else if (target == NEXT && page + 1 < pages) ++page;
        draw_page();
    } else if (target && target <= tasks.count &&
               !tablet_tasks_complete(&tasks.items[target - 1])) {
        *message_until = xTaskGetTickCount() + pdMS_TO_TICKS(4000);
    }
}

static void list_task(void *arg) {
    (void)arg;
    touch_init_calibrated();
    snprintf(last_status, sizeof(last_status), "%s", tablet_network_status());
    draw_home();
    checkbox_tap_t tap = {0};
    TickType_t message_until = 0;
    for (;;) {
        int touch_x, touch_y;
        bool down = touch_is_down();
        bool sampled = down && touch_read(&touch_x, &touch_y);
        unsigned hit = sampled ? hit_item(touch_x, touch_y) : 0;
        unsigned target = checkbox_tap_update(&tap, down, sampled, hit);
        if (target) handle_target(target, &message_until);
        if (tablet_tasks_take(&incoming)) {
            tap.target = 0;
            accept_snapshot(&incoming);
        }
        const char *status = message_until && (int32_t)(message_until - xTaskGetTickCount()) > 0
                            ? "OFFLINE: TRY AGAIN" : tablet_network_status();
        if (strcmp(last_status, status)) {
            snprintf(last_status, sizeof(last_status), "%s", status);
            update_status(status);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void ble_list_start(void) {
    display_init();
    display_clear();
    header("STARTING");
    ESP_ERROR_CHECK(xTaskCreate(list_task, "ble_list", 6144, NULL, 3, NULL)
                    == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
}
