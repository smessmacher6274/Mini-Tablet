#pragma once
#include "todo_model.h"
#include "wifi_settings.h"
bool tablet_network_set_wifi(const wifi_settings_t *settings);
void tablet_network_start(void);
bool tablet_network_take(todo_snapshot_t *snapshot);
bool tablet_network_complete(const todo_item_t *item);
const char *tablet_network_status(void);
