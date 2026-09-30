#include "display.h"
#include "ble.h"
#include "esp_log.h"
#include "touch.h"
#include "network.h"

void app_main(void) {
    // Initialize and draw once; never reset the display in a tight loop.
    display_hello_world();
    ESP_LOGI("tablet", "Portrait display initialized");
    tablet_ble_start();
    tablet_network_start();
    // NimBLE has its own task; this task owns all display/touch operations.
    touch_drawing_run();
}
