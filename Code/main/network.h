#pragma once
#include "todo_model.h"
void tablet_network_start(void);
bool tablet_network_take(todo_snapshot_t *snapshot);
const char *tablet_network_status(void);
