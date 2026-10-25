#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "usb_saber.h"
#include "ble_uart.h"

#define BUF_SIZE 2048

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    /* Un solo scrittore e un solo lettore per stream buffer (requisito di FreeRTOS):
     * to_saber: scrive la callback GATT, legge il task USB.
     * to_ble:   scrive la callback dati CDC, legge il task di notifica BLE. */
    StreamBufferHandle_t to_saber = xStreamBufferCreate(BUF_SIZE, 1);
    StreamBufferHandle_t to_ble   = xStreamBufferCreate(BUF_SIZE, 1);
    configASSERT(to_saber && to_ble);

    usb_saber_start(to_saber, to_ble);
    ble_uart_start(to_saber, to_ble);
}
