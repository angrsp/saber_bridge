idf_component_register(
    SRCS "main.c" "bridge.c" "pipe.c" "usb_saber.c" "ble_uart.c" "ble_status.c" "battery.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES bt nvs_flash esp_driver_gpio esp_adc esp_app_format
)
