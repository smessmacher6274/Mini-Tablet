#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define TODO_MAX_ITEMS 50
#define TODO_MAX_TITLE 120
#define TODO_MAX_MESSAGE 24576
typedef struct {
    char title[TODO_MAX_TITLE + 1];
    bool completed;
} todo_item_t;
typedef struct {
    int32_t revision;
    unsigned count;
    todo_item_t items[TODO_MAX_ITEMS];
} todo_snapshot_t;
bool todo_parse(const char *data, size_t length, todo_snapshot_t *output);
