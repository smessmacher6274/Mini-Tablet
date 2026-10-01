idf_component_register(SRCS "main.c" "ble.c" "display.c" "touch.c" "ui.c" "network.c" "todo_model.c" "local_tasks.c" "tablet_tasks.c" "wifi_settings.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_driver_gpio esp_driver_spi bt nvs_flash esp_timer esp_wifi esp_event esp_netif mqtt cjson)
