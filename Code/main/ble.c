#include "ble.h"
#include "tablet_tasks.h"
#include "network.h"
#include "test_mode.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nimble/nimble_npl.h"
#include "os/os_mempool.h"
#include "nvs_flash.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "tablet_ble";
static uint8_t address_type;
static bool connected;
static struct ble_npl_callout advertising_retry;
static struct ble_npl_callout transport_probe;
static uint16_t active_handle = BLE_HS_CONN_HANDLE_NONE;
static char last_task[TODO_MAX_TITLE + 1];
static int64_t last_task_at;
static unsigned local_count;
static unsigned greeting_reads, status_reads, read_errors;
static int64_t connected_at;

static void log_packet_pools(void) {
    struct os_mempool *pool = NULL;
    struct os_mempool_info info;
    while ((pool = os_mempool_info_get_next(pool, &info)) != NULL) {
        ESP_LOGI(TAG, "Pool %s free=%d/%d min=%d block=%d",
                 info.omi_name, info.omi_num_free, info.omi_num_blocks,
                 info.omi_min_free, info.omi_block_size);
    }
}

static void probe_transport(struct ble_npl_event *event) {
    (void)event;
    if (connected) {
        int8_t rssi = 0;
        int rc = ble_gap_conn_rssi(active_handle, &rssi);
        ESP_LOGI(TAG, "Host probe handle=%u rssi_rc=%d rssi=%d msys_free=%d reads=%u/%u stack_free=%u",
                 active_handle, rc, rssi, os_msys_num_free(),
                 greeting_reads, status_reads,
                 (unsigned)uxTaskGetStackHighWaterMark(NULL));
        log_packet_pools();
    }
    ble_npl_callout_reset(&transport_probe, ble_npl_time_ms_to_ticks32(10000));
}

static int append_read(struct os_mbuf *om, const char *value, size_t length) {
    int rc = os_mbuf_append(om, value, length);
    if (rc) ++read_errors;
    unsigned total = greeting_reads + status_reads;
    // Sample rather than logging every request during the read stress test.
    if (rc || (total && total % 32 == 0)) {
        ESP_LOGI(TAG, "Read stats greeting=%u status=%u errors=%u heap=%u min_heap=%u host_stack_free=%u",
                 greeting_reads, status_reads, read_errors,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT),
                 (unsigned)uxTaskGetStackHighWaterMark(NULL));
    }
    return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}
// NimBLE represents 128-bit UUIDs least-significant byte first.
static ble_uuid128_t service_uuid = BLE_UUID128_INIT(
    0xf0,0xde,0xbc,0x9a,0x78,0x56,0x34,0x12,
    0x78,0x56,0x34,0x12,0x78,0x56,0x34,0x12);
static const ble_uuid128_t greeting_uuid = BLE_UUID128_INIT(
    0xf1,0xde,0xbc,0x9a,0x78,0x56,0x34,0x12,
    0x78,0x56,0x34,0x12,0x78,0x56,0x34,0x12);
static const ble_uuid128_t add_task_uuid = BLE_UUID128_INIT(
    0xf2,0xde,0xbc,0x9a,0x78,0x56,0x34,0x12,
    0x78,0x56,0x34,0x12,0x78,0x56,0x34,0x12);
static const ble_uuid128_t status_uuid = BLE_UUID128_INIT(
    0xf3,0xde,0xbc,0x9a,0x78,0x56,0x34,0x12,
    0x78,0x56,0x34,0x12,0x78,0x56,0x34,0x12);
static const char *last_result = "READY";
#if !TABLET_BLE_TEST_MODE
static const ble_uuid128_t wifi_ssid_uuid = BLE_UUID128_INIT(
    0xf4,0xde,0xbc,0x9a,0x78,0x56,0x34,0x12,
    0x78,0x56,0x34,0x12,0x78,0x56,0x34,0x12);
static const ble_uuid128_t wifi_password_uuid = BLE_UUID128_INIT(
    0xf5,0xde,0xbc,0x9a,0x78,0x56,0x34,0x12,
    0x78,0x56,0x34,0x12,0x78,0x56,0x34,0x12);
static const ble_uuid128_t wifi_apply_uuid = BLE_UUID128_INIT(
    0xf6,0xde,0xbc,0x9a,0x78,0x56,0x34,0x12,
    0x78,0x56,0x34,0x12,0x78,0x56,0x34,0x12);
static const ble_uuid128_t wifi_status_uuid = BLE_UUID128_INIT(
    0xf7,0xde,0xbc,0x9a,0x78,0x56,0x34,0x12,
    0x78,0x56,0x34,0x12,0x78,0x56,0x34,0x12);
#endif
static wifi_settings_t staged_wifi;
static const char *wifi_result;

#if !TABLET_BLE_TEST_MODE
static int wifi_access(uint16_t connection, uint16_t attribute,
                       struct ble_gatt_access_ctxt *context, void *arg) {
    if (ble_uuid_cmp(context->chr->uuid, &wifi_status_uuid.u) == 0) {
        if (context->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_READ_NOT_PERMITTED;
        const char *value = wifi_result ? wifi_result : tablet_network_status();
        return os_mbuf_append(context->om, value, strlen(value)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (context->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
    char value[64] = {0};
    uint16_t length = 0;
    if (OS_MBUF_PKTLEN(context->om) > 63 ||
        ble_hs_mbuf_to_flat(context->om, value, 63, &length)) {
        wifi_settings_clear(&staged_wifi);
        wifi_result = "ERR LENGTH";
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    int result = 0;
    if (ble_uuid_cmp(context->chr->uuid, &wifi_ssid_uuid.u) == 0) {
        if (wifi_settings_ssid(&staged_wifi, value, length)) wifi_result = "SSID READY";
        else { wifi_result = "ERR SSID"; result = 0x80; }
    } else if (ble_uuid_cmp(context->chr->uuid, &wifi_password_uuid.u) == 0) {
        if (wifi_settings_password(&staged_wifi, value, length)) wifi_result = "PASSWORD READY";
        else { wifi_result = "ERR PASSWORD"; result = 0x80; }
    } else {
        if (length != 5 || memcmp(value, "APPLY", 5)) { wifi_result = "SEND APPLY"; result = 0x80; }
        else if (!wifi_settings_ready(&staged_wifi)) { wifi_result = "SET SSID PASSWORD"; result = 0x80; }
        else if (!tablet_network_set_wifi(&staged_wifi)) { wifi_result = "BUSY RETRY APPLY"; result = BLE_ATT_ERR_INSUFFICIENT_RES; }
        else { wifi_settings_clear(&staged_wifi); wifi_result = NULL; }
    }
    volatile char *secret = value;
    for (size_t n = 0; n < sizeof(value); ++n) secret[n] = 0;
    return result;
}
#endif

static int add_task(uint16_t connection, uint16_t attribute,
                    struct ble_gatt_access_ctxt *context, void *arg) {
    if (context->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
    unsigned length = OS_MBUF_PKTLEN(context->om);
    if (!length || length > TODO_MAX_TITLE) {
        last_result = "ERR LENGTH";
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    char text[TODO_MAX_TITLE + 1];
    uint16_t copied;
    if (ble_hs_mbuf_to_flat(context->om, text, TODO_MAX_TITLE, &copied) || copied != length)
        return BLE_ATT_ERR_UNLIKELY;
    text[length] = '\0';
    // A quick repeat from a testing app is a retry, not a second task.
    int64_t now = esp_timer_get_time();
    if (last_task_at && now - last_task_at < 2000000 &&
        !strcmp(last_task, text)) {
        last_result = "OK RETRY";
        return 0;
    }
    task_add_result_t result = tablet_tasks_add(text, length);
    if (result == TASK_ADD_INVALID) { last_result = "ERR TEXT"; return 0x80; }
    if (result == TASK_ADD_FULL) { last_result = "ERR FULL"; return BLE_ATT_ERR_INSUFFICIENT_RES; }
    last_result = "OK RAM";
    memcpy(last_task, text, length + 1);
    last_task_at = now;
    local_count = tablet_tasks_local_count();
    ESP_LOGI(TAG, "BLE task accepted in RAM (%u/%u)", local_count, TODO_LOCAL_ITEMS);
    return 0;
}

static int read_status(uint16_t connection, uint16_t attribute,
                       struct ble_gatt_access_ctxt *context, void *arg) {
    if (context->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_READ_NOT_PERMITTED;
    char value[32];
    // BLE is the only producer of local tasks; avoid a model mutex in reads.
    ++status_reads;
    snprintf(value, sizeof(value), "%s %u/%u", last_result, local_count, TODO_LOCAL_ITEMS);
    return append_read(context->om, value, strlen(value));
}

static void check_ble(int rc) {
    if (rc) {
        ESP_LOGE(TAG, "NimBLE error: %d", rc);
        abort();
    }
}

static int read_greeting(uint16_t connection, uint16_t attribute,
                         struct ble_gatt_access_ctxt *context, void *arg) {
    (void)connection; (void)attribute; (void)arg;
    if (context->op != BLE_GATT_ACCESS_OP_READ_CHR)
        return BLE_ATT_ERR_READ_NOT_PERMITTED;
    // Preserve the user's most recent Pico greeting.
    static const char greeting[] = "What it do";
    ++greeting_reads;
    return append_read(context->om, greeting, sizeof(greeting) - 1);
}

static const struct ble_gatt_svc_def services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &service_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = &greeting_uuid.u, .access_cb = read_greeting,
              .flags = BLE_GATT_CHR_F_READ },
            { .uuid = &add_task_uuid.u, .access_cb = add_task,
              .flags = BLE_GATT_CHR_F_WRITE },
            { .uuid = &status_uuid.u, .access_cb = read_status,
              .flags = BLE_GATT_CHR_F_READ },
#if !TABLET_BLE_TEST_MODE
            { .uuid = &wifi_ssid_uuid.u, .access_cb = wifi_access,
              .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC },
            { .uuid = &wifi_password_uuid.u, .access_cb = wifi_access,
              .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC },
            { .uuid = &wifi_apply_uuid.u, .access_cb = wifi_access,
              .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC },
            { .uuid = &wifi_status_uuid.u, .access_cb = wifi_access,
              .flags = BLE_GATT_CHR_F_READ },
#endif
            {0}
        }
    },
    {0}
};

static void advertise(void);
static void schedule_advertising(void) {
    ble_npl_callout_reset(&advertising_retry, ble_npl_time_ms_to_ticks32(250));
}
static void retry_advertising(struct ble_npl_event *event) {
    (void)event;
    advertise();
}
static void log_connection(uint16_t handle) {
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(handle, &desc)) return;
    ESP_LOGI(TAG, "Link handle=%u interval=%u (1.25ms) latency=%u timeout=%u (10ms) encrypted=%u bonded=%u",
             handle, desc.conn_itvl, desc.conn_latency, desc.supervision_timeout,
             desc.sec_state.encrypted, desc.sec_state.bonded);
}
static void request_connection_timing(uint16_t handle) {
    // 30-45 ms interval, no peripheral latency, 6 s supervision timeout.
    // The central decides whether to accept these parameters; log the result.
    const struct ble_gap_upd_params params = {
        .itvl_min = 24,
        .itvl_max = 36,
        .latency = 0,
        .supervision_timeout = 600,
    };
    int rc = ble_gap_update_params(handle, &params);
    if (rc) ESP_LOGW(TAG, "Connection timing request failed: %d", rc);
    else ESP_LOGI(TAG, "Requested 30-45ms interval, latency 0, timeout 6000ms");
}
static int gap_event(struct ble_gap_event *event, void *arg) {
    (void)arg;
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            ESP_LOGI(TAG, "Connection status: %d", event->connect.status);
            connected = event->connect.status == 0;
            if (connected) {
                active_handle = event->connect.conn_handle;
                connected_at = esp_timer_get_time();
                greeting_reads = status_reads = read_errors = 0;
                log_connection(event->connect.conn_handle);
                log_packet_pools();
                request_connection_timing(event->connect.conn_handle);
            }
            if (!connected) schedule_advertising();
            break;
        case BLE_GAP_EVENT_DISCONNECT:
            connected = false;
            active_handle = BLE_HS_CONN_HANDLE_NONE;
            wifi_settings_clear(&staged_wifi);
            wifi_result = NULL;
            ESP_LOGI(TAG, "Disconnected, reason %d", event->disconnect.reason);
            ESP_LOGI(TAG, "Link lasted %lld ms; greeting_reads=%u status_reads=%u read_errors=%u heap=%u min_heap=%u",
                     (long long)((esp_timer_get_time() - connected_at) / 1000),
                     greeting_reads, status_reads, read_errors,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                     (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
            if (event->disconnect.reason == BLE_HS_HCI_ERR(BLE_ERR_REM_USER_CONN_TERM))
                ESP_LOGW(TAG, "Peer ended link (may be automatic; not necessarily a user action)");
            else if (event->disconnect.reason == BLE_HS_HCI_ERR(BLE_ERR_CONN_SPVN_TMO))
                ESP_LOGW(TAG, "Link supervision timeout");
            schedule_advertising();
            log_packet_pools();
            break;
        case BLE_GAP_EVENT_ADV_COMPLETE:
            schedule_advertising();
            break;
        case BLE_GAP_EVENT_ENC_CHANGE:
            ESP_LOGI(TAG, "Encryption status: %d", event->enc_change.status);
            log_connection(event->enc_change.conn_handle);
            break;
        case BLE_GAP_EVENT_CONN_UPDATE:
            ESP_LOGI(TAG, "Connection update status: %d", event->conn_update.status);
            log_connection(event->conn_update.conn_handle);
            break;
        case BLE_GAP_EVENT_MTU:
            ESP_LOGI(TAG, "MTU handle=%u channel=%u value=%u",
                     event->mtu.conn_handle, event->mtu.channel_id, event->mtu.value);
            log_packet_pools();
            break;
        default: break;
    }
    return 0;
}

static void advertise(void) {
    if (connected || !ble_hs_synced() || ble_gap_adv_active()) return;
    struct ble_hs_adv_fields fields = {0};
    const char *name = ble_svc_gap_device_name();
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc) goto retry;
    // Service UUID goes in scan response to stay within the 31-byte limit.
    struct ble_hs_adv_fields response = {0};
    response.uuids128 = &service_uuid;
    response.num_uuids128 = 1;
    response.uuids128_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&response);
    if (rc) goto retry;
    struct ble_gap_adv_params params = {0};
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(address_type, NULL, BLE_HS_FOREVER,
                          &params, gap_event, NULL);
    if (rc) goto retry;
    ESP_LOGI(TAG, "Advertising as %s", name);
    return;
retry:
    ESP_LOGW(TAG, "Advertising failed (%d); retrying", rc);
    schedule_advertising();
}

static void on_sync(void) {
    check_ble(ble_hs_util_ensure_addr(0));
    check_ble(ble_hs_id_infer_auto(0, &address_type));
    advertise();
    if (TABLET_BLE_TEST_MODE)
        ble_npl_callout_reset(&transport_probe, ble_npl_time_ms_to_ticks32(10000));
}

static void on_reset(int reason) {
    connected = false;
    active_handle = BLE_HS_CONN_HANDLE_NONE;
    ble_npl_callout_stop(&transport_probe);
    ble_npl_callout_stop(&advertising_retry);
    wifi_settings_clear(&staged_wifi);
    wifi_result = NULL;
    ESP_LOGW(TAG, "Bluetooth host reset: %d", reason);
}

static void host_task(void *arg) {
    (void)arg;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void tablet_ble_start(void) {
    local_count = tablet_tasks_local_count();
    esp_err_t rc = nvs_flash_init();
    if (rc == ESP_ERR_NVS_NO_FREE_PAGES || rc == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // This prototype stores no user notes in NVS yet.
        ESP_ERROR_CHECK(nvs_flash_erase());
        rc = nvs_flash_init();
    }
    ESP_ERROR_CHECK(rc);
    ESP_ERROR_CHECK(nimble_port_init());
    ble_npl_callout_init(&advertising_retry, nimble_port_get_dflt_eventq(),
                        retry_advertising, NULL);
    ble_npl_callout_init(&transport_probe, nimble_port_get_dflt_eventq(),
                        probe_transport, NULL);
    check_ble(ble_att_set_preferred_mtu(185));
    ble_svc_gap_init();
    ble_svc_gatt_init();
    check_ble(ble_svc_gap_device_name_set("NoteTablet-BLE"));
    check_ble(ble_gatts_count_cfg(services));
    check_ble(ble_gatts_add_svcs(services));
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    // Encrypt credential writes with Just Works pairing; no persistent bonds yet.
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_bonding = 0;
    nimble_port_freertos_init(host_task);
}
