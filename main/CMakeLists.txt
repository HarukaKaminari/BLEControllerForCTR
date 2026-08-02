idf_component_register(SRCS "ble_ota.c" "ble_central.c" "ota_trigger.c" "status_led.c" "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES bt nvs_flash app_update led_strip)
