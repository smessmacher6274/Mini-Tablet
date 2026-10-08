#include "ble.h"
#include "esp_log.h"

void app_main(void) {
    ESP_LOGI("tablet", "Bluetooth-only isolation firmware");
    tablet_ble_start();
}
