#include "tablet_tasks.h"
#include "network.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_random.h"
#include <stdio.h>
#include <string.h>

static SemaphoreHandle_t lock;
static local_tasks_t local;
static todo_snapshot_t remote;
static bool changed;
static TickType_t last_retry;

void tablet_tasks_init(void) {
    lock = xSemaphoreCreateMutex();
    configASSERT(lock);
}

task_add_result_t tablet_tasks_add(const char *text, size_t length) {
    xSemaphoreTake(lock, portMAX_DELAY);
    task_add_result_t result = local_tasks_add(&local, text, length);
    if (result == TASK_ADD_OK) {
        todo_item_t *item = &local.items[local.count - 1];
        uint32_t a = esp_random(), b = esp_random(), c = esp_random(), d = esp_random();
        snprintf(item->id, sizeof(item->id), "%08lx-%04lx-4%03lx-%04lx-%04lx%08lx",
                 (unsigned long)a, (unsigned long)(b >> 16), (unsigned long)(b & 0xfff),
                 (unsigned long)((c >> 16 & 0x3fff) | 0x8000),
                 (unsigned long)(c & 0xffff), (unsigned long)d);
        tablet_network_create(item);
        changed = true;
    }
    xSemaphoreGive(lock);
    return result;
}

unsigned tablet_tasks_local_count(void) {
    xSemaphoreTake(lock, portMAX_DELAY);
    unsigned count = local.count;
    xSemaphoreGive(lock);
    return count;
}

bool tablet_tasks_take(todo_snapshot_t *out) {
    xSemaphoreTake(lock, portMAX_DELAY);
    if (tablet_network_take(&remote)) {
        changed = true;
        // A matching retained task is the application acknowledgement, not PUBACK.
        for (unsigned i = 0; i < local.count;) {
            const todo_item_t *saved = NULL;
            for (unsigned n = 0; n < remote.count; ++n)
                if (!strcmp(local.items[i].id, remote.items[n].id)) { saved = &remote.items[n]; break; }
            if (saved && saved->completed == local.items[i].completed) {
                memmove(&local.items[i], &local.items[i + 1],
                        (local.count - i - 1) * sizeof(local.items[0]));
                --local.count;
            } else {
                if (saved) tablet_network_complete(saved);
                ++i;
            }
        }
    }
    TickType_t now = xTaskGetTickCount();
    if ((TickType_t)(now - last_retry) >= pdMS_TO_TICKS(5000)) {
        last_retry = now;
        for (unsigned i = 0; i < local.count; ++i) {
            const todo_item_t *saved = NULL;
            for (unsigned n = 0; n < remote.count; ++n)
                if (!strcmp(local.items[i].id, remote.items[n].id)) { saved = &remote.items[n]; break; }
            if (!saved) tablet_network_create(&local.items[i]);
            else if (saved->completed != local.items[i].completed) tablet_network_complete(saved);
        }
    }
    bool result = changed;
    if (changed) { local_tasks_merge(&local, &remote, out); changed = false; }
    xSemaphoreGive(lock);
    return result;
}

bool tablet_tasks_complete(const todo_item_t *item) {
    if (!item->local) return tablet_network_complete(item);
    xSemaphoreTake(lock, portMAX_DELAY);
    bool result = local_tasks_complete(&local, item);
    if (result) changed = true;
    xSemaphoreGive(lock);
    return result;
}
