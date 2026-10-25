idf_component_register(
    SRCS "main.c" "bridge.c" "pipe.c" "usb_saber.c" "ble_uart.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES bt nvs_flash esp_driver_gpio
)
