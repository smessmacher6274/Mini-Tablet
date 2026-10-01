#pragma once
#include "local_tasks.h"
void tablet_tasks_init(void);
task_add_result_t tablet_tasks_add(const char *text, size_t length);
bool tablet_tasks_take(todo_snapshot_t *out);
bool tablet_tasks_complete(const todo_item_t *item);
unsigned tablet_tasks_local_count(void);
