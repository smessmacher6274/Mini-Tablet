#include "ble.h"
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nimble/nimble_npl.h"
#include "os/os_mempool.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "ble_only";
static uint8_t address_type;
static bool connected;
static unsigned writes, reads;
static char last_write[121];
static struct ble_npl_callout retry;
static void log_packet_pools(void) {
    struct os_mempool *pool = NULL;
    struct os_mempool_info info;
    while ((pool = os_mempool_info_get_next(pool, &info)) != NULL) {
        ESP_LOGI(TAG, "Pool %s free=%d/%d min=%d block=%d",
                 info.omi_name, info.omi_num_free, info.omi_num_blocks,
                 info.omi_min_free, info.omi_block_size);
    }
}
static void log_link(uint16_t handle) {
    struct ble_gap_conn_desc desc;
    int rc = ble_gap_conn_find(handle, &desc);
    if (!rc) {
        ESP_LOGI(TAG, "Link handle=%u interval_units=%u latency=%u timeout_units=%u",
                 handle, desc.conn_itvl, desc.conn_latency, desc.supervision_timeout);
    } else {
        ESP_LOGW(TAG, "Link lookup failed=%d", rc);
    }
}
#define UUID(suffix) BLE_UUID128_INIT(suffix,0xde,0xbc,0x9a,0x78,0x56,0x34,0x12, \
                                     0x78,0x56,0x34,0x12,0x78,0x56,0x34,0x12)
static ble_uuid128_t service_uuid = UUID(0xf0);
static const ble_uuid128_t greeting_uuid = UUID(0xf1);
static const ble_uuid128_t write_uuid = UUID(0xf2);
static const ble_uuid128_t status_uuid = UUID(0xf3);

static int access_value(uint16_t connection, uint16_t attribute,
                        struct ble_gatt_access_ctxt *ctx, void *arg) {
    (void)connection; (void)attribute; (void)arg;
    if (ble_uuid_cmp(ctx->chr->uuid, &write_uuid.u) == 0) {
        if (ctx->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
        uint16_t length = 0;
        unsigned expected = OS_MBUF_PKTLEN(ctx->om);
        if (!expected || expected > 120) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        char value[121];
        if (ble_hs_mbuf_to_flat(ctx->om, value, 120, &length) || length != expected)
            return BLE_ATT_ERR_UNLIKELY;
        memcpy(last_write, value, length);
        last_write[length] = '\0';
        ++writes;
        ESP_LOGI(TAG, "Write accepted count=%u bytes=%u", writes, length);
        log_packet_pools();
        return 0;
    }
    if (ctx->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_READ_NOT_PERMITTED;
    ++reads;
    bool greeting = ble_uuid_cmp(ctx->chr->uuid, &greeting_uuid.u) == 0;
    char status[48];
    snprintf(status, sizeof(status), "WRITES %u READS %u", writes, reads);
    int rc = greeting ? os_mbuf_append(ctx->om, "What it do", 10)
                      : os_mbuf_append(ctx->om, status, strlen(status));
    ESP_LOGI(TAG, "Read count=%u characteristic=%s append_rc=%d",
             reads, greeting ? "f1" : "f3", rc);
    log_packet_pools();
    return rc ? BLE_ATT_ERR_INSUFFICIENT_RES : 0;
}

static const struct ble_gatt_svc_def services[] = {
    { .type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &service_uuid.u,
      .characteristics = (struct ble_gatt_chr_def[]) {
          { .uuid = &greeting_uuid.u, .access_cb = access_value, .flags = BLE_GATT_CHR_F_READ },
          { .uuid = &write_uuid.u, .access_cb = access_value, .flags = BLE_GATT_CHR_F_WRITE },
          { .uuid = &status_uuid.u, .access_cb = access_value, .flags = BLE_GATT_CHR_F_READ },
          {0}
      } },
    {0}
};

static void advertise(void);
static void schedule_retry(void) {
    ble_npl_callout_reset(&retry, ble_npl_time_ms_to_ticks32(250));
}
static void retry_event(struct ble_npl_event *event) { (void)event; advertise(); }
static int gap_event(struct ble_gap_event *event, void *arg) {
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        connected = event->connect.status == 0;
        ESP_LOGI(TAG, "Connect status=%d", event->connect.status);
        if (connected) log_link(event->connect.conn_handle);
        log_packet_pools();
        if (!connected) schedule_retry();
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        connected = false;
        ESP_LOGI(TAG, "Disconnect reason=%d writes=%u reads=%u",
                 event->disconnect.reason, writes, reads);
        log_packet_pools();
        schedule_retry();
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE: schedule_retry(); break;
    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU=%u", event->mtu.value);
        break;
    case BLE_GAP_EVENT_CONN_UPDATE:
        ESP_LOGI(TAG, "Timing update status=%d", event->conn_update.status);
        log_link(event->conn_update.conn_handle);
        break;
    default: break;
    }
    return 0;
}
static void advertise(void) {
    if (connected || !ble_hs_synced() || ble_gap_adv_active()) return;
    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    const char *name = ble_svc_gap_device_name();
    fields.name = (uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&fields);
    if (!rc) {
        struct ble_hs_adv_fields response = {0};
        response.uuids128 = &service_uuid;
        response.num_uuids128 = 1;
        response.uuids128_is_complete = 1;
        rc = ble_gap_adv_rsp_set_fields(&response);
    }
    if (!rc) {
        struct ble_gap_adv_params params = {0};
        params.conn_mode = BLE_GAP_CONN_MODE_UND;
        params.disc_mode = BLE_GAP_DISC_MODE_GEN;
        rc = ble_gap_adv_start(address_type, NULL, BLE_HS_FOREVER, &params, gap_event, NULL);
    }
    if (rc) { ESP_LOGW(TAG, "Advertising failed=%d; retry", rc); schedule_retry(); }
    else ESP_LOGI(TAG, "Advertising as %s", name);
}
static void on_sync(void) {
    int rc = ble_hs_util_ensure_addr(0);
    if (!rc) rc = ble_hs_id_infer_auto(0, &address_type);
    if (rc) { ESP_LOGE(TAG, "Address initialization failed=%d", rc); return; }
    advertise();
}
static void on_reset(int reason) {
    connected = false;
    ble_npl_callout_stop(&retry);
    ESP_LOGW(TAG, "Host reset=%d", reason);
}
static void host_task(void *arg) {
    (void)arg;
    nimble_port_run();
    nimble_port_freertos_deinit();
}
void tablet_ble_start(void) {
    // Preserve NVS on initialization errors; no automatic erase.
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(nimble_port_init());
    ble_npl_callout_init(&retry, nimble_port_get_dflt_eventq(), retry_event, NULL);
    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_svc_gap_device_name_set("NoteTablet-BLE");
    if (!rc) rc = ble_gatts_count_cfg(services);
    if (!rc) rc = ble_gatts_add_svcs(services);
    if (rc) { ESP_LOGE(TAG, "GATT initialization failed=%d", rc); return; }
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 0;
    ble_hs_cfg.sm_mitm = 0;
    nimble_port_freertos_init(host_task);
}
