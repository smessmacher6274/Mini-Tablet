idf_component_register(SRCS "main.c" "ble_minimal.c" "display.c" "ble_list.c" "touch.c"
    "network.c" "todo_model.c" "tablet_tasks.c" "local_tasks.c" "wifi_settings.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES bt nvs_flash esp_driver_gpio esp_driver_spi esp_wifi esp_event
    esp_netif espressif__mqtt espressif__cjson)
