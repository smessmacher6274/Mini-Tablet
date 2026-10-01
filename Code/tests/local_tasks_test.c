#include "local_tasks.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static local_tasks_t local;
static todo_snapshot_t remote, merged;
int main(void) {
    assert(local_tasks_add(&local, " Buy milk ", 10) == TASK_ADD_OK);
    assert(local.count == 1 && !strcmp(local.items[0].title, "Buy milk"));
    assert(local.items[0].local && !local.items[0].completed);
    assert(local_tasks_add(&local, " ", 1) == TASK_ADD_INVALID);
    assert(local_tasks_add(&local, "x\0y", 3) == TASK_ADD_INVALID);
    assert(local_tasks_add(&local, "x\ny", 3) == TASK_ADD_INVALID);
    assert(local_tasks_add(&local, "\xc0\xaf", 2) == TASK_ADD_INVALID);
    assert(local_tasks_add(&local, "\xed\xa0\x80", 3) == TASK_ADD_INVALID);
    assert(local_tasks_add(&local, "\xf4\x90\x80\x80", 4) == TASK_ADD_INVALID);
    assert(local_tasks_add(&local, "\xe2\x82", 2) == TASK_ADD_INVALID);
    assert(local_tasks_add(&local, "caf\xc3\xa9", 5) == TASK_ADD_OK);
    todo_item_t item = local.items[0];
    assert(local_tasks_complete(&local, &item));
    assert(local.items[0].completed);
    assert(!local_tasks_complete(&local, &item)); // Stale duplicate.
    item = local.items[0];
    assert(local_tasks_complete(&local, &item));
    assert(!local.items[0].completed);
    remote.count = 1;
    strcpy(remote.items[0].title, "From computer");
    local_tasks_merge(&local, &remote, &merged);
    assert(merged.count == 3 && merged.items[0].local && !merged.items[2].local);
    remote.count = 0; // Server deletion/reconnect does not discard BLE tasks.
    local_tasks_merge(&local, &remote, &merged);
    assert(merged.count == 2);
    char title[122]; memset(title, 'a', sizeof(title));
    assert(local_tasks_add(&local, title, 121) == TASK_ADD_INVALID);
    assert(local_tasks_add(&local, title, 120) == TASK_ADD_OK);
    while (local.count < TODO_LOCAL_ITEMS) assert(local_tasks_add(&local, "Task", 4) == TASK_ADD_OK);
    assert(local_tasks_add(&local, "Full", 4) == TASK_ADD_FULL);
    remote.count = TODO_MAX_ITEMS;
    local_tasks_merge(&local, &remote, &merged);
    assert(merged.count == TODO_MAX_VISIBLE);
    puts("BLE text validation, offline completion, capacity and MQTT merge tests passed");
}
