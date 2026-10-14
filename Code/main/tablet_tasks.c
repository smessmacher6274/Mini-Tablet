#include "tablet_tasks.h"
#include "network.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_random.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TASK_STORE_PARTITION "task_store"
#define TASK_STORE_NAMESPACE "tasks"
#define TASK_STORE_KEY "state_v1"
#define TASK_STATE_MAGIC 0x5441534bU
#define TASK_STATE_VERSION 2
#define JOURNAL_LIMIT 20

typedef struct {
    tablet_command_t command;
    bool has_result;
    tablet_command_result_t result;
} journal_entry_t;
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t journal_count;
    bool has_server_snapshot;
    todo_snapshot_t server;
    journal_entry_t journal[JOURNAL_LIMIT];
} durable_state_t;

static const char *TAG = "tablet_tasks";
static SemaphoreHandle_t lock;
static durable_state_t state;
static bool storage_ready;
static bool changed;
static TickType_t last_retry;
static todo_snapshot_t received_snapshot;
static local_tasks_t scratch_local;
static bool received_snapshot_pending;
static TickType_t last_snapshot_attempt;
static TickType_t last_finalize_attempt;

static void finish_result(unsigned index);

static void make_uuid(char out[37]) {
    uint32_t a = esp_random(), b = esp_random(), c = esp_random(), d = esp_random();
    snprintf(out, 37, "%08lx-%04lx-4%03lx-%04lx-%04lx%08lx",
             (unsigned long)a, (unsigned long)(b >> 16), (unsigned long)(b & 0xfff),
             (unsigned long)((c >> 16 & 0x3fff) | 0x8000),
             (unsigned long)(c & 0xffff), (unsigned long)d);
}

static bool valid_uuid(const char *value, size_t capacity) {
    if (!value || strnlen(value, capacity) != 36) return false;
    for (unsigned i = 0; i < 36; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (value[i] != '-') return false;
        } else {
            char c = value[i];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
                return false;
        }
    }
    return true;
}

static bool save_state(void) {
    if (!storage_ready) return false;
    nvs_handle_t handle;
    esp_err_t err = nvs_open_from_partition(TASK_STORE_PARTITION, TASK_STORE_NAMESPACE,
                                             NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot open task storage: %s", esp_err_to_name(err));
        return false;
    }
    err = nvs_set_blob(handle, TASK_STORE_KEY, &state, sizeof(state));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err != ESP_OK) ESP_LOGE(TAG, "Cannot save task journal: %s", esp_err_to_name(err));
    return err == ESP_OK;
}

static bool load_state(void) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open_from_partition(TASK_STORE_PARTITION, TASK_STORE_NAMESPACE,
                                             NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return true;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Task storage unavailable: %s", esp_err_to_name(err));
        return false;
    }
    size_t size = sizeof(state);
    err = nvs_get_blob(handle, TASK_STORE_KEY, &state, &size);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return true;
    bool valid = err == ESP_OK && size == sizeof(state) && state.magic == TASK_STATE_MAGIC &&
        state.version == TASK_STATE_VERSION && state.journal_count <= JOURNAL_LIMIT &&
        state.server.count <= TODO_MAX_ITEMS &&
        state.server.revision >= 0 && (state.has_server_snapshot || state.server.count == 0);
    if (valid) {
        for (unsigned i = 0; i < state.server.count; ++i) {
            todo_item_t *item = &state.server.items[i];
            if (!valid_uuid(item->id, sizeof(item->id)) ||
                !memchr(item->title, '\0', sizeof(item->title)) || !item->title[0] || item->local ||
                item->revision < 0 || item->revision > state.server.revision) { valid = false; break; }
        }
    }
    if (valid) {
        for (unsigned i = 0; i < state.journal_count; ++i) {
            journal_entry_t *entry = &state.journal[i];
            tablet_command_t *cmd = &entry->command;
            if (!valid_uuid(cmd->command_id, sizeof(cmd->command_id)) ||
                (cmd->op != TABLET_COMMAND_CREATE && cmd->op != TABLET_COMMAND_UPDATE) ||
                !valid_uuid(cmd->item.id, sizeof(cmd->item.id)) ||
                !memchr(cmd->item.title, '\0', sizeof(cmd->item.title)) || !cmd->item.title[0] ||
                (cmd->op == TABLET_COMMAND_UPDATE && cmd->base_revision < 0) ||
                (entry->has_result && (entry->result.status < TABLET_RESULT_APPLIED ||
                 entry->result.status > TABLET_RESULT_REJECTED ||
                 !valid_uuid(entry->result.command_id, sizeof(entry->result.command_id)) ||
                 entry->result.revision < 0 ||
                 strcmp(entry->result.command_id, cmd->command_id)))) {
                valid = false; break;
            }
        }
    }
    if (!valid) {
        memset(&state, 0, sizeof(state));
        state.magic = TASK_STATE_MAGIC;
        state.version = TASK_STATE_VERSION;
        ESP_LOGE(TAG, "Task storage record is invalid or incompatible; preserving it and rejecting writes");
        return false;
    }
    return true;
}

static int find_item(const todo_snapshot_t *snapshot, const char *id) {
    for (unsigned i = 0; i < snapshot->count; ++i)
        if (!strcmp(snapshot->items[i].id, id)) return (int)i;
    return -1;
}

static int find_command(const char *id) {
    for (unsigned i = 0; i < state.journal_count; ++i)
        if (!strcmp(state.journal[i].command.command_id, id)) return (int)i;
    return -1;
}

static void remove_journal(unsigned index) {
    if (index + 1 < state.journal_count)
        memmove(&state.journal[index], &state.journal[index + 1],
                (state.journal_count - index - 1) * sizeof(state.journal[0]));
    --state.journal_count;
    memset(&state.journal[state.journal_count], 0, sizeof(state.journal[0]));
}

static int latest_update(const char *id) {
    for (int i = (int)state.journal_count - 1; i >= 0; --i)
        if (state.journal[i].command.op == TABLET_COMMAND_UPDATE &&
            !strcmp(state.journal[i].command.item.id, id)) return i;
    return -1;
}

static bool add_update(const todo_item_t *item, bool completed, int32_t base_revision) {
    if (state.journal_count == JOURNAL_LIMIT) return false;
    journal_entry_t *entry = &state.journal[state.journal_count++];
    memset(entry, 0, sizeof(*entry));
    tablet_command_t *cmd = &entry->command;
    make_uuid(cmd->command_id);
    cmd->op = TABLET_COMMAND_UPDATE;
    cmd->item = *item;
    cmd->base_revision = base_revision;
    cmd->completed = completed;
    return true;
}

static unsigned pending_creates(void) {
    unsigned count = 0;
    for (unsigned i = 0; i < state.journal_count; ++i)
        if (state.journal[i].command.op == TABLET_COMMAND_CREATE) ++count;
    return count;
}

void tablet_tasks_init(void) {
    lock = xSemaphoreCreateMutex();
    configASSERT(lock);
    memset(&state, 0, sizeof(state));
    state.magic = TASK_STATE_MAGIC;
    state.version = TASK_STATE_VERSION;
    last_retry = 0;
    received_snapshot_pending = false;
    last_snapshot_attempt = 0;
    last_finalize_attempt = 0;
    esp_err_t err = nvs_flash_init_partition(TASK_STORE_PARTITION);
    if (err == ESP_OK) {
        storage_ready = load_state();
        tablet_network_set_cached_revision(state.server.revision);
    } else {
        storage_ready = false;
        ESP_LOGE(TAG, "Durable task storage unavailable: %s; task writes will be rejected", esp_err_to_name(err));
    }
    changed = true;
}

task_add_result_t tablet_tasks_add(const char *text, size_t length) {
    xSemaphoreTake(lock, portMAX_DELAY);
    if (state.journal_count >= JOURNAL_LIMIT || pending_creates() >= TODO_LOCAL_ITEMS) {
        xSemaphoreGive(lock);
        return TASK_ADD_FULL;
    }
    memset(&scratch_local, 0, sizeof(scratch_local));
    task_add_result_t result = local_tasks_add(&scratch_local, text, length);
    if (result == TASK_ADD_OK) {
        todo_item_t item = scratch_local.items[0];
        make_uuid(item.id);
        item.local = true;
        journal_entry_t *entry = &state.journal[state.journal_count++];
        memset(entry, 0, sizeof(*entry));
        entry->command.op = TABLET_COMMAND_CREATE;
        entry->command.item = item;
        entry->command.completed = false;
        make_uuid(entry->command.command_id);
        if (!save_state()) {
            --state.journal_count;
            result = TASK_ADD_FULL; // No durable capacity is available; never claim acceptance.
        } else changed = true;
    }
    xSemaphoreGive(lock);
    return result;
}

unsigned tablet_tasks_local_count(void) {
    xSemaphoreTake(lock, portMAX_DELAY);
    unsigned count = pending_creates();
    xSemaphoreGive(lock);
    return count;
}

static bool apply_server_snapshot(const todo_snapshot_t *snapshot) {
    if (state.has_server_snapshot && snapshot->revision == state.server.revision) return true;
    durable_state_t *previous = malloc(sizeof(state));
    if (!previous) return false;
    memcpy(previous, &state, sizeof(state));
    state.server = *snapshot;
    state.has_server_snapshot = true;
    if (save_state()) { free(previous); return true; }
    memcpy(&state, previous, sizeof(state));
    free(previous);
    ESP_LOGW(TAG, "Keeping prior task cache because new snapshot could not be saved");
    return false;
}

static bool persist_received_snapshot(void) {
    if (!received_snapshot_pending) return true;
    last_snapshot_attempt = xTaskGetTickCount();
    if (received_snapshot.revision < state.server.revision) return false;
    if (!apply_server_snapshot(&received_snapshot)) return false;
    received_snapshot_pending = false;
    changed = true;
    for (unsigned i = 0; i < state.journal_count;) {
        if (state.journal[i].has_result && state.journal[i].result.revision <= state.server.revision) {
            finish_result(i);
            if (i < state.journal_count && state.journal[i].has_result &&
                state.journal[i].result.revision <= state.server.revision) ++i;
        } else ++i;
    }
    return true;
}

static void finish_result(unsigned index) {
    if (index >= state.journal_count || !state.journal[index].has_result ||
        state.journal[index].result.revision > state.server.revision) return;
    durable_state_t *previous = malloc(sizeof(state));
    if (!previous) return;
    memcpy(previous, &state, sizeof(state));
    tablet_command_result_t result = state.journal[index].result;
    tablet_command_t command = state.journal[index].command;
    if (result.status == TABLET_RESULT_APPLIED) {
        // A create can be acknowledged after a later server deletion; the result still closes it.
        remove_journal(index);
        if (command.op == TABLET_COMMAND_CREATE) {
            int created = find_item(&state.server, command.item.id);
            if (created >= 0 && command.completed != state.server.items[created].completed &&
                !add_update(&state.server.items[created], command.completed, result.revision)) {
                memcpy(&state, previous, sizeof(state));
                free(previous);
                return;
            }
        } else {
            for (unsigned i = 0; i < state.journal_count; ++i) {
                tablet_command_t *next = &state.journal[i].command;
                if (next->op == TABLET_COMMAND_UPDATE && !strcmp(next->item.id, command.item.id) &&
                    next->base_revision < result.revision) next->base_revision = result.revision;
            }
        }
    } else {
        // A rejection/conflict ends the chain for this task. Fresh server state remains authoritative.
        for (unsigned i = 0; i < state.journal_count;) {
            tablet_command_t *pending = &state.journal[i].command;
            if (!strcmp(pending->item.id, command.item.id)) remove_journal(i);
            else ++i;
        }
    }
    if (!save_state()) memcpy(&state, previous, sizeof(state));
    else changed = true;
    free(previous);
}

static void handle_result(const tablet_command_result_t *result) {
    int index = find_command(result->command_id);
    if (index < 0) return; // Duplicate acknowledgement after reboot or retry.
    durable_state_t *previous = malloc(sizeof(state));
    if (!previous) return;
    memcpy(previous, &state, sizeof(state));
    state.journal[index].has_result = true;
    state.journal[index].result = *result;
    if (!save_state()) {
        memcpy(&state, previous, sizeof(state));
        free(previous);
        return;
    }
    free(previous);
    if (!received_snapshot_pending && result->revision <= state.server.revision) finish_result((unsigned)index);
}

static void finalize_ready_results(void) {
    last_finalize_attempt = xTaskGetTickCount();
    for (unsigned i = 0; i < state.journal_count;) {
        if (!received_snapshot_pending && state.journal[i].has_result &&
            state.journal[i].result.revision <= state.server.revision) {
            finish_result(i);
            if (i < state.journal_count && state.journal[i].has_result &&
                state.journal[i].result.revision <= state.server.revision) ++i;
        } else ++i;
    }
}

static void replay_one(void) {
    if (!state.journal_count || received_snapshot_pending || !tablet_network_snapshot_ready()) return;
    // Preserve per-task order: a later edit waits for the prior result.
    tablet_command_t *cmd = &state.journal[0].command;
    if (state.journal[0].has_result) return;
    if (cmd->op == TABLET_COMMAND_UPDATE) {
        // Keep the revision captured when the offline edit was made. A newer server
        // revision must surface as a conflict instead of silently overwriting it.
    }
    tablet_network_send_command(cmd);
}

bool tablet_tasks_take(todo_snapshot_t *out) {
    xSemaphoreTake(lock, portMAX_DELAY);
    if (tablet_network_take(&received_snapshot)) {
        received_snapshot_pending = true;
        last_snapshot_attempt = xTaskGetTickCount() - pdMS_TO_TICKS(5000);
    }
    TickType_t now = xTaskGetTickCount();
    if (received_snapshot_pending && (TickType_t)(now - last_snapshot_attempt) >= pdMS_TO_TICKS(5000))
        persist_received_snapshot();
    tablet_command_result_t result;
    while (tablet_network_take_result(&result)) handle_result(&result);
    if ((TickType_t)(now - last_finalize_attempt) >= pdMS_TO_TICKS(5000)) finalize_ready_results();
    if ((TickType_t)(now - last_retry) >= pdMS_TO_TICKS(5000)) {
        last_retry = now;
        replay_one();
    }
    if (changed) {
        memset(&scratch_local, 0, sizeof(scratch_local));
        for (unsigned i = 0; i < state.journal_count; ++i) {
            tablet_command_t *cmd = &state.journal[i].command;
            if (cmd->op == TABLET_COMMAND_CREATE && scratch_local.count < TODO_LOCAL_ITEMS) {
                scratch_local.items[scratch_local.count] = cmd->item;
                scratch_local.items[scratch_local.count++].completed = cmd->completed;
            }
        }
        local_tasks_merge(&scratch_local, &state.server, out);
        for (unsigned i = 0; i < state.journal_count; ++i) {
            tablet_command_t *cmd = &state.journal[i].command;
            if (cmd->op != TABLET_COMMAND_UPDATE) continue;
            int n = find_item(out, cmd->item.id);
            if (n >= 0) out->items[n].completed = cmd->completed;
        }
        changed = false;
        xSemaphoreGive(lock);
        return true;
    }
    xSemaphoreGive(lock);
    return false;
}

bool tablet_tasks_complete(const todo_item_t *item) {
    if (!item) return false;
    xSemaphoreTake(lock, portMAX_DELAY);
    bool result = false;
    int create = -1;
    for (unsigned i = 0; i < state.journal_count; ++i)
        if (state.journal[i].command.op == TABLET_COMMAND_CREATE &&
            !strcmp(state.journal[i].command.item.id, item->id)) { create = (int)i; break; }
    durable_state_t *previous = malloc(sizeof(state));
    if (!previous) { xSemaphoreGive(lock); return false; }
    memcpy(previous, &state, sizeof(state));
    if (create >= 0) {
        state.journal[create].command.completed = !item->completed;
        state.journal[create].command.item.completed = !item->completed;
        result = true;
    } else {
        int latest = latest_update(item->id);
        int server = find_item(&state.server, item->id);
        if (server >= 0 && state.journal_count < JOURNAL_LIMIT) {
            bool current = latest >= 0 ? state.journal[latest].command.completed : state.server.items[server].completed;
            result = add_update(&state.server.items[server], !current,
                                latest >= 0 ? state.journal[latest].command.base_revision : state.server.items[server].revision);
        }
    }
    if (result && save_state()) changed = true;
    else { memcpy(&state, previous, sizeof(state)); result = false; }
    free(previous);
    xSemaphoreGive(lock);
    return result;
}
