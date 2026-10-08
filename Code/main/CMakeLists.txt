idf_component_register(SRCS "main.c" "ble_minimal.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES bt nvs_flash)
