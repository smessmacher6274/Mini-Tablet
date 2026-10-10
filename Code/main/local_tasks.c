#include "local_tasks.h"
#include <stdio.h>
#include <string.h>

// Reject controls, malformed UTF-8, overlong encodings, surrogates and NUL.
static bool valid_text(const unsigned char *text, size_t length) {
    for (size_t i = 0; i < length;) {
        unsigned c = text[i++], extra = 0, minimum = 0;
        if (c < 128) { if (c < 32 || c == 127) return false; continue; }
        if (c >= 0xc2 && c <= 0xdf) { c &= 31; extra = 1; minimum = 0x80; }
        else if (c >= 0xe0 && c <= 0xef) { c &= 15; extra = 2; minimum = 0x800; }
        else if (c >= 0xf0 && c <= 0xf4) { c &= 7; extra = 3; minimum = 0x10000; }
        else return false;
        if (i + extra > length) return false;
        while (extra--) {
            unsigned next = text[i++];
            if ((next & 0xc0) != 0x80) return false;
            c = (c << 6) | (next & 63);
        }
        if (c < minimum || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff) ||
            (c >= 0x80 && c <= 0x9f) || c == 0x2028 || c == 0x2029) return false;
    }
    return true;
}

task_add_result_t local_tasks_add(local_tasks_t *tasks, const char *text, size_t length) {
    if (!text || !length || length > TODO_MAX_TITLE ||
        !valid_text((const unsigned char *)text, length)) return TASK_ADD_INVALID;
    while (length && *text == ' ') { ++text; --length; }
    while (length && text[length - 1] == ' ') --length;
    if (!length) return TASK_ADD_INVALID;
    if (tasks->count == TODO_LOCAL_ITEMS) return TASK_ADD_FULL;
    todo_item_t *item = &tasks->items[tasks->count];
    memset(item, 0, sizeof(*item));
    snprintf(item->id, sizeof(item->id), "ble-%u", tasks->count + 1);
    memcpy(item->title, text, length);
    item->local = true;
    ++tasks->count;
    return TASK_ADD_OK;
}

bool local_tasks_complete(local_tasks_t *tasks, const todo_item_t *item) {
    if (!item->local) return false;
    for (unsigned n = 0; n < tasks->count; ++n) {
        todo_item_t *current = &tasks->items[n];
        if (!strcmp(current->id, item->id) && current->revision == item->revision) {
            current->completed = !item->completed;
            ++current->revision;
            return true;
        }
    }
    return false;
}

void local_tasks_merge(const local_tasks_t *local, const todo_snapshot_t *remote, todo_snapshot_t *out) {
    out->revision = remote->revision;
    out->count = 0;
    // Local portable tasks first, then the unchanged server list.
    for (unsigned n = 0; n < local->count; ++n) out->items[out->count++] = local->items[n];
    for (unsigned n = 0; n < remote->count && n < TODO_MAX_ITEMS; ++n) {
        bool pending = false;
        for (unsigned i = 0; i < local->count; ++i)
            if (!strcmp(local->items[i].id, remote->items[n].id)) { pending = true; break; }
        if (!pending) out->items[out->count++] = remote->items[n];
    }
}
