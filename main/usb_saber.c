#include "esp_log.h"
#include "esp_intr_alloc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usb/usb_host.h"
#include "usb/cdc_acm_host.h"
#include "usb_saber.h"

static const char *TAG = "usb_saber";

#define TX_CHUNK 128   /* al massimo 128 byte per scrittura verso la NXT (_CHUNK_SIZE di py2saber) */

static StreamBufferHandle_t s_tx_in;    /* BLE -> spada */
static StreamBufferHandle_t s_rx_out;   /* spada -> BLE */
static volatile bool s_connected;
static uint32_t s_dropped;              /* byte persi per buffer pieno */

bool usb_saber_connected(void) { return s_connected; }

/* Chiamata dal driver CDC quando arrivano dati dalla spada. */
static bool on_data(const uint8_t *data, size_t len, void *arg)
{
    size_t sent = xStreamBufferSend(s_rx_out, data, len, 0);
    if (sent < len) {
        s_dropped += len - sent;
    }
    return true;   /* dati consumati */
}

static void on_event(const cdc_acm_host_dev_event_data_t *event, void *user_ctx)
{
    switch (event->type) {
    case CDC_ACM_HOST_ERROR:
        ESP_LOGE(TAG, "errore CDC: %d", event->data.error);
        break;
    case CDC_ACM_HOST_DEVICE_DISCONNECTED:
        ESP_LOGW(TAG, "spada scollegata");
        s_connected = false;      /* il task chiude l'handle */
        break;
    default:
        break;
    }
}

static void usb_lib_task(void *arg)
{
    for (;;) {
        uint32_t flags;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            ESP_ERROR_CHECK(usb_host_device_free_all());
        }
    }
}

static void saber_task(void *arg)
{
    const cdc_acm_host_device_config_t dev_cfg = {
        .connection_timeout_ms = 5000,
        .out_buffer_size = 512,
        .event_cb = on_event,
        .data_cb = on_data,
        .user_arg = NULL,
    };
    const cdc_acm_line_coding_t coding = {
        .dwDTERate = 1152000, .bCharFormat = 0, .bParityType = 0, .bDataBits = 8,
    };
    uint8_t buf[TX_CHUNK];

    for (;;) {
        cdc_acm_dev_hdl_t dev = NULL;

        /* Qualsiasi VID/PID: si restringe in F2 (NXT 0x0483/0x5740, EVO 0x16C0/0x0483). */
        esp_err_t err = cdc_acm_host_open(CDC_HOST_ANY_VID, CDC_HOST_ANY_PID, 0, &dev_cfg, &dev);
        if (err != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        ESP_LOGI(TAG, "spada collegata");

        (void)cdc_acm_host_line_coding_set(dev, &coding);
        (void)cdc_acm_host_set_control_line_state(dev, true, true);   /* DTR + RTS, come pyserial */

        /* scarta i byte vecchi accumulati mentre la spada non c'era */
        while (xStreamBufferReceive(s_tx_in, buf, sizeof buf, 0) > 0) { }
        s_connected = true;

        while (s_connected) {
            size_t n = xStreamBufferReceive(s_tx_in, buf, sizeof buf, pdMS_TO_TICKS(50));
            if (n > 0 && cdc_acm_host_data_tx_blocking(dev, buf, n, 1000) != ESP_OK) {
                ESP_LOGW(TAG, "scrittura verso la spada fallita");
            }
        }
        cdc_acm_host_close(dev);   /* va chiamata anche dopo l'evento di disconnessione */
    }
}

void usb_saber_start(StreamBufferHandle_t to_saber, StreamBufferHandle_t to_ble)
{
    s_tx_in = to_saber;
    s_rx_out = to_ble;

    const usb_host_config_t host_cfg = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    ESP_ERROR_CHECK(usb_host_install(&host_cfg));
    xTaskCreate(usb_lib_task, "usb_lib", 4096, NULL, 10, NULL);
    ESP_ERROR_CHECK(cdc_acm_host_install(NULL));
    xTaskCreate(saber_task, "saber", 4096, NULL, 5, NULL);
}
