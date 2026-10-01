#pragma once
#include "todo_model.h"
typedef struct {
    unsigned count;
    todo_item_t items[TODO_LOCAL_ITEMS];
} local_tasks_t;
typedef enum { TASK_ADD_OK, TASK_ADD_INVALID, TASK_ADD_FULL } task_add_result_t;
task_add_result_t local_tasks_add(local_tasks_t *tasks, const char *text, size_t length);
bool local_tasks_complete(local_tasks_t *tasks, const todo_item_t *item);
void local_tasks_merge(const local_tasks_t *local, const todo_snapshot_t *remote, todo_snapshot_t *out);
