#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../main/ble_list.c"

typedef struct { unsigned x, y, w, h; unsigned short color; } rect_call_t;
typedef struct { unsigned x, y, scale; unsigned short color; char text[64]; } label_call_t;
static rect_call_t rects[256];
static label_call_t labels[256];
static unsigned rect_count, label_count;
static unsigned recalibrations;

static void events_clear(void) { rect_count = label_count = 0; }
static bool has_label(const char *text) {
    for (unsigned i = 0; i < label_count; ++i)
        if (!strcmp(labels[i].text, text)) return true;
    return false;
}
static bool has_rect(unsigned y, unsigned h) {
    for (unsigned i = 0; i < rect_count; ++i)
        if (rects[i].y == y && rects[i].h == h && rects[i].x == 0 && rects[i].w == 320)
            return true;
    return false;
}
static todo_snapshot_t snapshot(unsigned count, const char **ids, const char **titles) {
    todo_snapshot_t result = {0};
    result.count = count;
    for (unsigned i = 0; i < count; ++i) {
        snprintf(result.items[i].id, sizeof(result.items[i].id), "%s", ids[i]);
        snprintf(result.items[i].title, sizeof(result.items[i].title), "%s", titles[i]);
    }
    return result;
}
static void reset_ui(void) {
    memset(&tasks, 0, sizeof(tasks));
    memset(rendered, 0, sizeof(rendered));
    page = first = visible_count = footer_page = footer_pages = 0;
    pages = 1;
    rendered_placeholder = false;
    screen = SCREEN_HOME;
    snprintf(last_status, sizeof(last_status), "ONLINE");
    events_clear();
}

void display_init(void) {}
void display_clear(void) {}
void display_target(int x, int y) { (void)x; (void)y; }
void display_toolbar(bool erasing) { (void)erasing; }
void display_stroke(int a, int b, int c, int d, bool e) { (void)a;(void)b;(void)c;(void)d;(void)e; }
void display_note_restore(void) {}
void display_rect(unsigned x, unsigned y, unsigned w, unsigned h, unsigned short color) {
    if (rect_count < 256) rects[rect_count++] = (rect_call_t){x,y,w,h,color};
}
void display_label(unsigned x, unsigned y, const char *s, unsigned scale, unsigned short color) {
    if (label_count < 256) {
        label_call_t *call = &labels[label_count++];
        call->x=x; call->y=y; call->scale=scale; call->color=color;
        snprintf(call->text, sizeof(call->text), "%s", s);
    }
}
void touch_init_calibrated(void) {}
void touch_recalibrate(void) { ++recalibrations; }
bool touch_read(int *x, int *y) { (void)x; (void)y; return false; }
bool touch_is_down(void) { return false; }
task_add_result_t tablet_tasks_add(const char *text, size_t length) {
    (void)text; (void)length; return TASK_ADD_OK;
}
bool tablet_tasks_take(todo_snapshot_t *out) { (void)out; return false; }
bool tablet_tasks_complete(const todo_item_t *item) { (void)item; return true; }
unsigned tablet_tasks_local_count(void) { return 0; }
const char *tablet_network_status(void) { return "ONLINE"; }
TickType_t xTaskGetTickCount(void) { return 0; }
void vTaskDelay(TickType_t ticks) { (void)ticks; }
int xTaskCreate(void (*task)(void *), const char *name, unsigned stack,
                void *arg, unsigned priority, TaskHandle_t *handle) {
    (void)task;(void)name;(void)stack;(void)arg;(void)priority;(void)handle; return pdPASS;
}

int main(void) {
    TickType_t message_until = 0;
    const char *ids1[] = {"id-a"}, *titles1[] = {"alpha"};
    const char *ids2[] = {"id-a", "id-b"}, *titles2[] = {"alpha", "beta"};
    const char *ids3[] = {"id-a", "id-b", "id-c"}, *titles3[] = {"alpha", "beta", "gamma"};
    reset_ui();
    accept_snapshot(&(todo_snapshot_t){0});
    draw_home();
    assert(has_label("HOME") && !has_label("TO DO / 0"));
    assert(hit_item(100, 200) == SHOW_LIST);
    assert(hit_item(100, 300) == RECALIBRATE);
    assert(hit_item(270, 100) == 0); // Home header has no Home button.
    handle_target(RECALIBRATE, &message_until);
    assert(recalibrations == 1 && screen == SCREEN_HOME && has_label("HOME"));

    todo_snapshot_t one = snapshot(1, ids1, titles1);
    accept_snapshot(&one);
    assert(has_label("1 TASKS") && !has_label("alpha")); // Home updates silently.
    handle_target(SHOW_LIST, &message_until);
    assert(screen == SCREEN_LIST && has_label("TO DO / 1") && has_label("HOME") && has_label("ONLINE"));
    assert(hit_item(260, 10) == HOME);
    events_clear();
    todo_snapshot_t two = snapshot(2, ids2, titles2);
    accept_snapshot(&two);
    assert(has_label("TO DO / 2") && has_label("beta") && !has_label("alpha"));
    assert(has_rect(TOP + 28, 28)); // Append clears and draws only the new row.

    events_clear();
    todo_snapshot_t three = snapshot(3, ids3, titles3);
    accept_snapshot(&three);
    events_clear();
    three.items[1].completed = true;
    accept_snapshot(&three);
    assert(has_label("beta") && has_label("X"));
    assert(!has_label("alpha") && !has_label("gamma")); // Completion repaints only row two.

    events_clear();
    todo_snapshot_t two_again = snapshot(2, ids2, titles2);
    two_again.items[1].completed = true;
    accept_snapshot(&two_again);
    assert(has_rect(TOP + 2 * 28, 28)); // Removing the last row erases its old footprint only.
    assert(!has_label("alpha") && !has_label("beta"));

    events_clear();
    todo_snapshot_t empty = {0};
    accept_snapshot(&empty);
    assert(has_rect(TOP, 2 * 28) && has_label("NO TASKS YET"));
    events_clear();
    accept_snapshot(&one);
    assert(has_rect(TOP, 28) && has_label("alpha") && !has_label("NO TASKS YET"));

    // Page-local cache slots still identify rows on later pages.
    const char *many_ids[15], *many_titles[15];
    char ids[15][16], titles[15][16];
    for (unsigned i = 0; i < 15; ++i) {
        snprintf(ids[i], sizeof(ids[i]), "row-id-%u", i);
        snprintf(titles[i], sizeof(titles[i]), "row-%u", i);
        many_ids[i] = ids[i]; many_titles[i] = titles[i];
    }
    todo_snapshot_t many = snapshot(15, many_ids, many_titles);
    accept_snapshot(&many);
    page = 1;
    paginate();
    draw_page();
    assert(first == 13 && visible_count == 2);
    events_clear();
    many.items[14].completed = true;
    accept_snapshot(&many);
    assert(has_label("row-14") && has_label("X") && !has_label("row-13"));

    events_clear();
    handle_target(HOME, &message_until);
    assert(screen == SCREEN_HOME && has_label("HOME") && has_label("15 TASKS") && has_label("ONLINE"));
    assert(!has_label("row-13") && !has_label("row-14"));
    puts("BLE list renderer tests passed");
}
