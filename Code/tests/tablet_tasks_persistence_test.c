#include "tablet_tasks.h"
#include "network.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char nvs_blob[32768];
static size_t nvs_blob_size;
static bool nvs_has_blob, nvs_fail_write;
static uint32_t random_seed = 10;
static TickType_t ticks;
static bool network_ready, pending_snapshot;
static todo_snapshot_t network_snapshot;
static tablet_command_result_t result_queue[20];
static unsigned result_count;
static tablet_command_t sent[64];
static unsigned sent_count;
static unsigned committed_writes;

esp_err_t nvs_flash_init_partition(const char *partition) { (void)partition; return ESP_OK; }
esp_err_t nvs_open_from_partition(const char *partition, const char *name,
                                  nvs_open_mode_t mode, nvs_handle_t *handle) {
    (void)partition; (void)name; (void)mode; *handle = 1; return ESP_OK;
}
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out, size_t *size) {
    (void)handle; (void)key;
    if (!nvs_has_blob) return ESP_ERR_NVS_NOT_FOUND;
    if (*size < nvs_blob_size) { *size = nvs_blob_size; return -1; }
    memcpy(out, nvs_blob, nvs_blob_size); *size = nvs_blob_size; return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *data, size_t size) {
    (void)handle; (void)key;
    if (nvs_fail_write || size > sizeof(nvs_blob)) return -1;
    memcpy(nvs_blob, data, size); nvs_blob_size = size; nvs_has_blob = true; return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t handle) {
    (void)handle; if (nvs_fail_write) return -1; ++committed_writes; return ESP_OK;
}
void nvs_close(nvs_handle_t handle) { (void)handle; }
const char *esp_err_to_name(esp_err_t err) { (void)err; return "mock error"; }
uint32_t esp_random(void) { return ++random_seed; }
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return malloc(1); }
int xSemaphoreTake(SemaphoreHandle_t mutex, TickType_t ticks_to_wait) { (void)mutex; (void)ticks_to_wait; return pdTRUE; }
int xSemaphoreGive(SemaphoreHandle_t mutex) { (void)mutex; return pdTRUE; }
TickType_t xTaskGetTickCount(void) { return ticks; }

bool tablet_network_set_wifi(const wifi_settings_t *settings) { (void)settings; return false; }
void tablet_network_start(void) { }
bool tablet_network_take(todo_snapshot_t *snapshot) {
    if (!pending_snapshot) return false;
    *snapshot = network_snapshot; pending_snapshot = false; return true;
}
bool tablet_network_take_result(tablet_command_result_t *result) {
    if (!result_count) return false;
    *result = result_queue[0];
    memmove(result_queue, result_queue + 1, (--result_count) * sizeof(result_queue[0]));
    return true;
}
bool tablet_network_snapshot_ready(void) { return network_ready; }
void tablet_network_set_cached_revision(int32_t revision) { (void)revision; }
bool tablet_network_send_command(const tablet_command_t *command) {
    if (!network_ready || sent_count == sizeof(sent) / sizeof(sent[0])) return false;
    sent[sent_count++] = *command; return true;
}
const char *tablet_network_status(void) { return "MOCK"; }

static void reset_network(void) {
    network_ready = false; pending_snapshot = false; result_count = 0;
    memset(&network_snapshot, 0, sizeof(network_snapshot));
    sent_count = 0; ticks = 0;
}
static void boot(void) { network_ready = false; tablet_tasks_init(); }
static void deliver_snapshot(int revision, const char *id, int item_revision, bool completed) {
    memset(&network_snapshot, 0, sizeof(network_snapshot));
    network_snapshot.revision = revision;
    if (id) {
        network_snapshot.count = 1;
        strcpy(network_snapshot.items[0].id, id);
        strcpy(network_snapshot.items[0].title, "Server task");
        network_snapshot.items[0].revision = item_revision;
        network_snapshot.items[0].completed = completed;
    }
    pending_snapshot = true;
}
static void deliver_result(const tablet_command_t *command, tablet_result_status_t status, int revision) {
    assert(result_count < 20);
    tablet_command_result_t *result = &result_queue[result_count++];
    strcpy(result->command_id, command->command_id);
    result->status = status; result->revision = revision;
}

static void test_create_and_checkbox_survive_reboot(void) {
    nvs_has_blob = false; nvs_fail_write = false; nvs_blob_size = 0; reset_network(); boot();
    assert(tablet_tasks_add("Offline create", 14) == TASK_ADD_OK);
    todo_snapshot_t view;
    assert(tablet_tasks_take(&view) && view.count == 1 && view.items[0].local && !view.items[0].completed);
    assert(tablet_tasks_complete(&view.items[0]));
    assert(tablet_tasks_take(&view) && view.items[0].completed);
    boot(); // Simulated reboot: reload the committed NVS blob.
    assert(tablet_tasks_take(&view) && view.count == 1 && view.items[0].local && view.items[0].completed);
}

static void test_stale_edit_waits_for_snapshot_and_keeps_base(void) {
    nvs_has_blob = false; nvs_fail_write = false; reset_network(); boot();
    const char *id = "12345678-1234-4234-8234-123456789abc";
    deliver_snapshot(5, id, 5, false);
    todo_snapshot_t view;
    assert(tablet_tasks_take(&view) && view.count == 1);
    assert(tablet_tasks_complete(&view.items[0]));
    boot();
    assert(tablet_tasks_take(&view) && view.items[0].completed); // Offline server checkbox survives reboot.
    deliver_snapshot(6, id, 6, false);
    assert(tablet_tasks_take(&view));
    network_ready = true; ticks += 5000;
    tablet_tasks_take(&view); // Replay is asynchronous to the visible snapshot update.
    assert(sent_count == 1 && sent[0].op == TABLET_COMMAND_UPDATE);
    assert(sent[0].base_revision == 5 && sent[0].completed);
    deliver_result(&sent[0], TABLET_RESULT_APPLIED, 7);
    assert(!tablet_tasks_take(&view)); // Ack alone cannot clear the overlay ahead of server state.
    boot();
    deliver_snapshot(6, id, 6, false);
    network_ready = true;
    ticks += 5000;
    assert(tablet_tasks_take(&view) && sent_count == 1); // Pending result survives reboot and blocks resend.
    deliver_snapshot(7, id, 7, true);
    assert(tablet_tasks_take(&view) && view.items[0].completed);
    deliver_result(&sent[0], TABLET_RESULT_APPLIED, 7); // Duplicate acknowledgement is harmless.
    assert(!tablet_tasks_take(&view));
}

static void test_deleted_task_update_is_sent_for_rejection(void) {
    nvs_has_blob = false; reset_network(); boot();
    const char *id = "abcdefab-cdef-4abc-8def-abcdefabcdef";
    deliver_snapshot(5, id, 5, false);
    todo_snapshot_t view;
    assert(tablet_tasks_take(&view) && view.count == 1);
    assert(tablet_tasks_complete(&view.items[0]));
    deliver_snapshot(6, NULL, 0, false);
    assert(tablet_tasks_take(&view) && view.count == 0);
    network_ready = true; ticks += 5000;
    tablet_tasks_take(&view);
    assert(sent_count == 1 && !strcmp(sent[0].item.id, id) && sent[0].base_revision == 5);
}

static void test_storage_failure_does_not_accept_create(void) {
    nvs_has_blob = false; reset_network(); boot();
    nvs_fail_write = true;
    assert(tablet_tasks_add("Not saved", 9) == TASK_ADD_FULL);
    assert(tablet_tasks_local_count() == 0);
}

static void test_failed_snapshot_save_blocks_replay_until_retry(void) {
    nvs_has_blob = false; nvs_fail_write = false; reset_network(); boot();
    assert(tablet_tasks_add("Queued", 6) == TASK_ADD_OK);
    network_ready = true;
    deliver_snapshot(1, NULL, 0, false);
    nvs_fail_write = true;
    ticks = 5000;
    todo_snapshot_t view;
    assert(tablet_tasks_take(&view));
    assert(sent_count == 0); // The fresh revision was received but not durably cached.
    nvs_fail_write = false;
    ticks += 5000;
    assert(tablet_tasks_take(&view)); // Retry persistence, then replay against that snapshot.
    assert(sent_count == 1 && sent[0].op == TABLET_COMMAND_CREATE);
}

int main(void) {
    test_create_and_checkbox_survive_reboot();
    test_stale_edit_waits_for_snapshot_and_keeps_base();
    test_deleted_task_update_is_sent_for_rejection();
    test_storage_failure_does_not_accept_create();
    test_failed_snapshot_save_blocks_replay_until_retry();
    assert(nvs_blob_size < 64 * 1024);
    printf("Tablet task persistence and command journal tests passed (blob=%zu bytes, commits=%u)\n",
           nvs_blob_size, committed_writes);
    return 0;
}
