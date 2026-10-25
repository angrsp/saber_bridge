if(CONFIG_BRIDGE_FAKE_SABER)
    set(saber_src "fake_saber.c" "nxt_emu.c")
else()
    set(saber_src "usb_saber.c")
endif()

idf_component_register(
    SRCS "main.c" "bridge.c" "pipe.c" ${saber_src} "ble_uart.c" "ble_status.c" "battery.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES bt nvs_flash esp_driver_gpio esp_driver_usb_serial_jtag esp_adc esp_app_format
)
