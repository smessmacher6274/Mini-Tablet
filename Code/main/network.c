#include "network.h"
#include "test_mode.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "mqtt_client.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "network_config.h"

_Static_assert(sizeof(TABLET_WIFI_SSID) > 1 && sizeof(TABLET_WIFI_SSID) <= 33,
               "Run setup_network.py: Wi-Fi SSID must be 1-32 bytes");
_Static_assert(sizeof(TABLET_WIFI_PASSWORD) <= 64, "Wi-Fi password too long");
_Static_assert(sizeof(TABLET_MQTT_URI) > 1,
               "Run setup_network.py: MQTT configuration is missing");

#define WIFI_READY BIT0
#define MQTT_READY BIT1
#define WIFI_FAILED BIT2
#define WIFI_APPLYING BIT3
#define WIFI_SERVICE_READY BIT4
#define SNAPSHOT_READY BIT5
ESP_EVENT_DEFINE_BASE(TABLET_WIFI_EVENT);
static bool switching_wifi;
static unsigned wifi_failures;
static const char *TAG = "tablet_net";
static const char *STATE_TOPIC = "notepad/v1/devices/tablet-001/state";
static const char *PRESENCE_TOPIC = "notepad/v1/devices/tablet-001/presence";
static const char *COMMAND_TOPIC = "notepad/v1/devices/tablet-001/commands";
static const char *RESULTS_TOPIC = "notepad/v1/devices/tablet-001/results";
static QueueHandle_t updates;
static QueueHandle_t results;
static EventGroupHandle_t status_bits;
static esp_mqtt_client_handle_t mqtt;
static char *incoming;
static size_t expected, received;
static unsigned incoming_kind;
static int32_t accepted_revision = -1;
static portMUX_TYPE revision_lock = portMUX_INITIALIZER_UNLOCKED;
static bool configured;

static void presence(void) {
    if (!mqtt || !(xEventGroupGetBits(status_bits) & MQTT_READY)) return;
    char payload[80];
    portENTER_CRITICAL(&revision_lock);
    int32_t revision = accepted_revision;
    portEXIT_CRITICAL(&revision_lock);
    snprintf(payload, sizeof(payload), "{\"online\":true,\"revision\":%ld}", (long)revision);
    esp_mqtt_client_enqueue(mqtt, PRESENCE_TOPIC, payload, 0, 1, 1, true);
}

static void reset_incoming(void) {
    free(incoming); incoming = NULL; expected = received = 0; incoming_kind = 0;
}

static bool parse_result(const char *data, size_t length, tablet_command_result_t *out) {
    if (!data || !out || !length || strlen(data) != length) return false;
    cJSON *root = cJSON_ParseWithLengthOpts(data, length + 1, NULL, true);
    if (!root) return false;
    cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "command_id");
    cJSON *status = cJSON_GetObjectItemCaseSensitive(root, "status");
    cJSON *revision = cJSON_GetObjectItemCaseSensitive(root, "revision");
    bool valid = cJSON_IsString(id) && strlen(id->valuestring) == 36 &&
        cJSON_IsString(status) && cJSON_IsNumber(revision) && revision->valueint >= 0 &&
        revision->valuedouble == revision->valueint && revision->valueint <= INT32_MAX;
    if (valid) {
        memcpy(out->command_id, id->valuestring, 37);
        out->revision = revision->valueint;
        if (!strcmp(status->valuestring, "applied")) out->status = TABLET_RESULT_APPLIED;
        else if (!strcmp(status->valuestring, "conflict")) out->status = TABLET_RESULT_CONFLICT;
        else if (!strcmp(status->valuestring, "rejected")) out->status = TABLET_RESULT_REJECTED;
        else valid = false;
    }
    cJSON_Delete(root);
    return valid;
}

static void mqtt_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    esp_mqtt_event_handle_t e = data;
    if (id == MQTT_EVENT_CONNECTED) {
        xEventGroupClearBits(status_bits, SNAPSHOT_READY);
        xEventGroupSetBits(status_bits, MQTT_READY);
        ESP_LOGI(TAG, "MQTT connected; waiting for fresh task list");
        esp_mqtt_client_subscribe(mqtt, STATE_TOPIC, 1);
        esp_mqtt_client_subscribe(mqtt, RESULTS_TOPIC, 1);
        presence();
    } else if (id == MQTT_EVENT_DISCONNECTED) {
        xEventGroupClearBits(status_bits, MQTT_READY);
        reset_incoming();
        ESP_LOGW(TAG, "MQTT disconnected; cached tasks retained");
    } else if (id == MQTT_EVENT_DATA) {
        if (e->current_data_offset == 0) {
            reset_incoming();
            if (e->topic_len == strlen(STATE_TOPIC) && !memcmp(e->topic, STATE_TOPIC, e->topic_len)) incoming_kind = 1;
            else if (e->topic_len == strlen(RESULTS_TOPIC) && !memcmp(e->topic, RESULTS_TOPIC, e->topic_len)) incoming_kind = 2;
            else return;
            if (e->total_data_len <= 0 || e->total_data_len > TODO_MAX_MESSAGE) { incoming_kind = 0; return; }
            expected = e->total_data_len;
            incoming = malloc(expected + 1);
            if (!incoming) { expected = 0; ESP_LOGE(TAG, "No memory for task update"); return; }
        }
        if (!incoming) return;
        if (e->current_data_offset != received || e->data_len < 0 ||
            received + e->data_len > expected || e->total_data_len != expected) {
            reset_incoming(); return;
        }
        memcpy(incoming + received, e->data, e->data_len);
        received += e->data_len;
        if (received == expected) {
            incoming[received] = '\0';
            if (incoming_kind == 1) {
              todo_snapshot_t *snapshot = calloc(1, sizeof(*snapshot));
              if (snapshot && todo_parse(incoming, received, snapshot)) {
                bool first = !(xEventGroupGetBits(status_bits) & SNAPSHOT_READY);
                portENTER_CRITICAL(&revision_lock);
                bool newer = (first && snapshot->revision >= accepted_revision) ||
                             snapshot->revision > accepted_revision;
                if (newer) accepted_revision = snapshot->revision;
                portEXIT_CRITICAL(&revision_lock);
                if (newer) {
                    xQueueOverwrite(updates, snapshot);
                    xEventGroupSetBits(status_bits, SNAPSHOT_READY);
                    ESP_LOGI(TAG, "Received %u tasks, revision %ld", snapshot->count, (long)snapshot->revision);
                }
                presence();
              } else ESP_LOGW(TAG, "Rejected invalid task snapshot");
              free(snapshot);
            } else {
                tablet_command_result_t result;
                if (parse_result(incoming, received, &result)) xQueueSend(results, &result, 0);
                else ESP_LOGW(TAG, "Rejected invalid command result");
            }
            reset_incoming();
        }
    } else if (id == MQTT_EVENT_ERROR) {
        ESP_LOGW(TAG, "MQTT connection error: check address, credentials and firewall");
    }
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == TABLET_WIFI_EVENT) {
        wifi_settings_t *settings = data;
        switching_wifi = true;
        wifi_failures = 0;
        xEventGroupClearBits(status_bits, WIFI_READY | MQTT_READY | WIFI_FAILED);
        xEventGroupSetBits(status_bits, WIFI_APPLYING);
        wifi_config_t wifi = {0};
        memcpy(wifi.sta.ssid, settings->ssid, strlen(settings->ssid));
        memcpy(wifi.sta.password, settings->password, strlen(settings->password));
        wifi.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        esp_err_t rc = esp_wifi_stop();
        if (rc == ESP_OK) rc = esp_wifi_set_config(WIFI_IF_STA, &wifi);
        if (rc == ESP_OK) rc = esp_wifi_start();
        volatile unsigned char *secret = (volatile unsigned char *)&wifi;
        for (size_t n = 0; n < sizeof(wifi); ++n) secret[n] = 0;
        wifi_settings_clear(settings);
        if (rc != ESP_OK) {
            switching_wifi = false;
            xEventGroupClearBits(status_bits, WIFI_APPLYING);
            xEventGroupSetBits(status_bits, WIFI_FAILED);
            ESP_LOGW(TAG, "Wi-Fi configuration failed: %s", esp_err_to_name(rc));
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        switching_wifi = false;
        xEventGroupClearBits(status_bits, WIFI_APPLYING);
        esp_wifi_connect();
    }
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(status_bits, WIFI_READY | MQTT_READY | SNAPSHOT_READY);
        if (switching_wifi) return;
        if (++wifi_failures >= 5) xEventGroupSetBits(status_bits, WIFI_FAILED);
        wifi_event_sta_disconnected_t *event = data;
        ESP_LOGW(TAG, "Wi-Fi disconnected (reason %u); retrying", event->reason);
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        if (switching_wifi) return;
        wifi_failures = 0;
        xEventGroupClearBits(status_bits, WIFI_FAILED | WIFI_APPLYING);
        xEventGroupSetBits(status_bits, WIFI_READY);
        ESP_LOGI(TAG, "Wi-Fi ready");
    }
}

static void connection_task(void *arg) {
    xEventGroupWaitBits(status_bits, WIFI_READY, pdFALSE, pdTRUE, portMAX_DELAY);
    esp_mqtt_client_config_t config = {
        .broker.address.uri = TABLET_MQTT_URI,
        .credentials.username = TABLET_MQTT_USER[0] ? TABLET_MQTT_USER : NULL,
        .credentials.authentication.password = TABLET_MQTT_PASSWORD[0] ? TABLET_MQTT_PASSWORD : NULL,
        .credentials.client_id = "note-tablet-001",
        .session.keepalive = 30,
        .session.last_will.topic = "notepad/v1/devices/tablet-001/presence",
        .session.last_will.msg = "{\"online\":false}",
        .session.last_will.qos = 1,
        .session.last_will.retain = 1,
        .network.reconnect_timeout_ms = 5000,
        .task.stack_size = 8192,
        .buffer.size = 2048,
    };
    mqtt = esp_mqtt_client_init(&config);
    if (!mqtt) { ESP_LOGE(TAG, "MQTT allocation failed"); vTaskDelete(NULL); return; }
    ESP_ERROR_CHECK(esp_mqtt_client_register_event(mqtt, ESP_EVENT_ANY_ID, mqtt_event, NULL));
    ESP_ERROR_CHECK(esp_mqtt_client_start(mqtt));
    for (;;) { vTaskDelay(pdMS_TO_TICKS(15000)); presence(); }
}

void tablet_network_start(void) {
    updates = xQueueCreate(1, sizeof(todo_snapshot_t));
    results = xQueueCreate(20, sizeof(tablet_command_result_t));
    status_bits = xEventGroupCreate();
    if (!updates || !results || !status_bits) { ESP_LOGE(TAG, "Network allocation failed"); return; }
    configured = strlen(TABLET_WIFI_SSID) && strlen(TABLET_MQTT_URI);
    if (!configured) { ESP_LOGW(TAG, "Run python3 setup_network.py before flashing to enable Wi-Fi"); return; }
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(TABLET_WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL));
    wifi_config_t wifi = {0};
    memcpy(wifi.sta.ssid, TABLET_WIFI_SSID, strlen(TABLET_WIFI_SSID));
    memcpy(wifi.sta.password, TABLET_WIFI_PASSWORD, strlen(TABLET_WIFI_PASSWORD));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi));
    ESP_ERROR_CHECK(esp_wifi_start());
    if (xTaskCreate(connection_task, "tablet_mqtt", 4096, NULL, 4, NULL) != pdPASS)
        ESP_LOGE(TAG, "Could not start network task");
    xEventGroupSetBits(status_bits, WIFI_SERVICE_READY);
}

bool tablet_network_set_wifi(const wifi_settings_t *settings) {
    if (!configured || !status_bits || !wifi_settings_ready(settings)) return false;
    EventBits_t bits = xEventGroupGetBits(status_bits);
    if (!(bits & WIFI_SERVICE_READY) || (bits & WIFI_APPLYING)) return false;
    xEventGroupSetBits(status_bits, WIFI_APPLYING);
    // Copy into the event queue; never stop/start Wi-Fi in the BLE host task.
    bool sent = esp_event_post(TABLET_WIFI_EVENT, 0, settings, sizeof(*settings), 0) == ESP_OK;
    if (!sent) xEventGroupClearBits(status_bits, WIFI_APPLYING);
    return sent;
}

bool tablet_network_take(todo_snapshot_t *snapshot) {
    return updates && xQueueReceive(updates, snapshot, 0) == pdTRUE;
}

bool tablet_network_take_result(tablet_command_result_t *result) {
    return results && xQueueReceive(results, result, 0) == pdTRUE;
}

bool tablet_network_snapshot_ready(void) {
    return status_bits && (xEventGroupGetBits(status_bits) & (MQTT_READY | SNAPSHOT_READY)) == (MQTT_READY | SNAPSHOT_READY);
}

void tablet_network_set_cached_revision(int32_t revision) {
    portENTER_CRITICAL(&revision_lock);
    if (revision > accepted_revision) accepted_revision = revision;
    portEXIT_CRITICAL(&revision_lock);
}

bool tablet_network_send_command(const tablet_command_t *request) {
    if (!request || !mqtt || !tablet_network_snapshot_ready()) return false;
    cJSON *command = cJSON_CreateObject();
    if (!command) return false;
    bool valid;
    if (request->op == TABLET_COMMAND_CREATE) {
        valid = cJSON_AddStringToObject(command, "op", "create") &&
                cJSON_AddStringToObject(command, "request_id", request->item.id) &&
                cJSON_AddStringToObject(command, "title", request->item.title) &&
                cJSON_AddStringToObject(command, "command_id", request->command_id);
    } else {
        valid = cJSON_AddStringToObject(command, "op", "update") &&
                cJSON_AddStringToObject(command, "id", request->item.id) &&
                cJSON_AddNumberToObject(command, "base_revision", request->base_revision) &&
                cJSON_AddBoolToObject(command, "completed", request->completed) &&
                cJSON_AddStringToObject(command, "command_id", request->command_id);
    }
    char *payload = valid ? cJSON_PrintUnformatted(command) : NULL;
    bool sent = payload && esp_mqtt_client_enqueue(mqtt,
        COMMAND_TOPIC, payload, 0, 1, 0, false) >= 0;
    cJSON_free(payload);
    cJSON_Delete(command);
    return sent;
}

const char *tablet_network_status(void) {
    if (!configured || !status_bits) return "WIFI NOT SET";
    EventBits_t bits = xEventGroupGetBits(status_bits);
    if (bits & WIFI_APPLYING) return "WIFI APPLYING";
    if (bits & WIFI_FAILED) return "WIFI FAILED RETRY";
    if (!(bits & WIFI_READY)) return "WIFI CONNECTING";
    if (!(bits & MQTT_READY)) return "MQTT CONNECTING";
    return "MQTT CONNECTED";
}
