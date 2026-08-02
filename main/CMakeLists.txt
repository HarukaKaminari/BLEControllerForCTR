idf_component_register(SRCS "ble_ota.c" "ota_trigger.c" "status_led.c" "blink_example_main.c"
                       INCLUDE_DIRS "."
                       REQUIRES bt nvs_flash app_update led_strip)
