#include "ble.h"
#include "tablet_tasks.h"
#include "network.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "tablet_ble";
static uint8_t address_type;
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
static wifi_settings_t staged_wifi;
static const char *wifi_result;

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
    task_add_result_t result = tablet_tasks_add(text, length);
    if (result == TASK_ADD_INVALID) { last_result = "ERR TEXT"; return 0x80; }
    if (result == TASK_ADD_FULL) { last_result = "ERR FULL"; return BLE_ATT_ERR_INSUFFICIENT_RES; }
    last_result = "OK RAM";
    ESP_LOGI(TAG, "BLE task accepted in RAM (%u/%u)", tablet_tasks_local_count(), TODO_LOCAL_ITEMS);
    return 0;
}

static int read_status(uint16_t connection, uint16_t attribute,
                       struct ble_gatt_access_ctxt *context, void *arg) {
    if (context->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_READ_NOT_PERMITTED;
    char value[32];
    snprintf(value, sizeof(value), "%s %u/%u", last_result, tablet_tasks_local_count(), TODO_LOCAL_ITEMS);
    return os_mbuf_append(context->om, value, strlen(value)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
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
    return os_mbuf_append(context->om, greeting, sizeof(greeting) - 1) == 0
        ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
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
            { .uuid = &wifi_ssid_uuid.u, .access_cb = wifi_access,
              .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC },
            { .uuid = &wifi_password_uuid.u, .access_cb = wifi_access,
              .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC },
            { .uuid = &wifi_apply_uuid.u, .access_cb = wifi_access,
              .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC },
            { .uuid = &wifi_status_uuid.u, .access_cb = wifi_access,
              .flags = BLE_GATT_CHR_F_READ },
            {0}
        }
    },
    {0}
};

static void advertise(void);
static int gap_event(struct ble_gap_event *event, void *arg) {
    (void)arg;
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            ESP_LOGI(TAG, "Connection status: %d", event->connect.status);
            if (event->connect.status != 0) advertise();
            break;
        case BLE_GAP_EVENT_DISCONNECT:
            wifi_settings_clear(&staged_wifi);
            wifi_result = NULL;
            ESP_LOGI(TAG, "Disconnected, reason %d", event->disconnect.reason);
            advertise();
            break;
        case BLE_GAP_EVENT_ADV_COMPLETE:
            advertise();
            break;
        default: break;
    }
    return 0;
}

static void advertise(void) {
    struct ble_hs_adv_fields fields = {0};
    const char *name = ble_svc_gap_device_name();
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;
    check_ble(ble_gap_adv_set_fields(&fields));
    // Service UUID goes in scan response to stay within the 31-byte limit.
    struct ble_hs_adv_fields response = {0};
    response.uuids128 = &service_uuid;
    response.num_uuids128 = 1;
    response.uuids128_is_complete = 1;
    check_ble(ble_gap_adv_rsp_set_fields(&response));
    struct ble_gap_adv_params params = {0};
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    check_ble(ble_gap_adv_start(address_type, NULL, BLE_HS_FOREVER,
                                &params, gap_event, NULL));
    ESP_LOGI(TAG, "Advertising as %s", name);
}

static void on_sync(void) {
    check_ble(ble_hs_util_ensure_addr(0));
    check_ble(ble_hs_id_infer_auto(0, &address_type));
    advertise();
}

static void on_reset(int reason) {
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
    esp_err_t rc = nvs_flash_init();
    if (rc == ESP_ERR_NVS_NO_FREE_PAGES || rc == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // This prototype stores no user notes in NVS yet.
        ESP_ERROR_CHECK(nvs_flash_erase());
        rc = nvs_flash_init();
    }
    ESP_ERROR_CHECK(rc);
    ESP_ERROR_CHECK(nimble_port_init());
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
