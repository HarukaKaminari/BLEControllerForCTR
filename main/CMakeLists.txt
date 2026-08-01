idf_component_register(SRCS "ble_ota.c" "blink_example_main.c"
                       INCLUDE_DIRS "."
                       REQUIRES bt nvs_flash app_update)
