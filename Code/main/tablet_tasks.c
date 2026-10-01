#include "tablet_tasks.h"
#include "network.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static SemaphoreHandle_t lock;
static local_tasks_t local;
static todo_snapshot_t remote;
static bool changed;

void tablet_tasks_init(void) {
    lock = xSemaphoreCreateMutex();
    configASSERT(lock);
}

task_add_result_t tablet_tasks_add(const char *text, size_t length) {
    xSemaphoreTake(lock, portMAX_DELAY);
    task_add_result_t result = local_tasks_add(&local, text, length);
    if (result == TASK_ADD_OK) changed = true;
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
    if (tablet_network_take(&remote)) changed = true;
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
