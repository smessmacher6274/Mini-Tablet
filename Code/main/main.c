#include "ble.h"
#include "ble_list.h"
#include "tablet_tasks.h"
#include "network.h"
#include "esp_log.h"

void app_main(void) {
    ESP_LOGI("tablet", "BLE and MQTT touchscreen task firmware");
    tablet_tasks_init();
    ble_list_start();
    tablet_ble_start();
    tablet_network_start();
}
