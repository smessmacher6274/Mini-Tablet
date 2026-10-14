#pragma once
#include "todo_model.h"
#include "wifi_settings.h"

typedef enum { TABLET_RESULT_APPLIED, TABLET_RESULT_CONFLICT, TABLET_RESULT_REJECTED } tablet_result_status_t;
typedef struct {
    char command_id[37];
    tablet_result_status_t status;
    int32_t revision;
} tablet_command_result_t;
typedef enum { TABLET_COMMAND_CREATE, TABLET_COMMAND_UPDATE } tablet_command_op_t;
typedef struct {
    char command_id[37];
    tablet_command_op_t op;
    todo_item_t item;
    int32_t base_revision;
    bool completed;
} tablet_command_t;

bool tablet_network_set_wifi(const wifi_settings_t *settings);
void tablet_network_start(void);
bool tablet_network_take(todo_snapshot_t *snapshot);
bool tablet_network_take_result(tablet_command_result_t *result);
bool tablet_network_snapshot_ready(void);
void tablet_network_set_cached_revision(int32_t revision);
bool tablet_network_send_command(const tablet_command_t *command);
const char *tablet_network_status(void);
