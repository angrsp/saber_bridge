#include <stdatomic.h>
#include "bridge_log.h"
#include "esp_intr_alloc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "usb/usb_host.h"
#include "usb/cdc_acm_host.h"
#include "bridge.h"
#include "usb_saber.h"

static const char *TAG = "usb_saber";

#define TX_CHUNK 128   /* al massimo 128 byte per scrittura verso la NXT (_CHUNK_SIZE di py2saber) */

#define OPEN_TIMEOUT_MS 500
#define BACKOFF_MIN_MS  500
#define BACKOFF_MAX_MS  2000

#define USB_ID(vid, pid) (((uint32_t)(vid) << 16) | (pid))
#define ID_VID(id)       ((uint16_t)((id) >> 16))
#define ID_PID(id)       ((uint16_t)((id) & 0xFFFF))

#ifdef CONFIG_BRIDGE_ASSERT_DTR
#define ASSERT_DTR true
#else
#define ASSERT_DTR false
#endif
#ifdef CONFIG_BRIDGE_ASSERT_RTS
#define ASSERT_RTS true
#else
#define ASSERT_RTS false
#endif

#define ID_NXT USB_ID(0x0483, 0x5740)
#define ID_EVO USB_ID(0x16C0, 0x0483)

static pipe_t *s_tx_in;     /* BLE -> spada */
static pipe_t *s_rx_out;    /* spada -> BLE */
static TaskHandle_t s_task;
static volatile bool s_connected;
static volatile bool s_gone;
static volatile uint32_t s_open_id;  /* USB_ID della spada aperta */      /* evento di disconnessione arrivato per il dispositivo aperto */
static _Atomic uint32_t s_seen;   /* USB_ID dell'ultimo dispositivo comparso, 0 = nessuno */

bool usb_saber_connected(void) { return s_connected; }

bool usb_saber_ids(uint16_t *vid, uint16_t *pid)
{
    uint32_t id = s_connected ? s_open_id : 0;
    *vid = ID_VID(id);
    *pid = ID_PID(id);
    return id != 0;
}

static bool id_allowed(uint32_t id)
{
#if CONFIG_BRIDGE_SABER_ANY_DEVICE
    return true;
#else
    return id == ID_NXT || id == ID_EVO;
#endif
}

/* Contesto host USB: qui non si può aprire il dispositivo, si annota solo chi è comparso. */
static void on_new_dev(usb_device_handle_t usb_dev)
{
    const usb_device_desc_t *desc;
    if (usb_host_get_device_descriptor(usb_dev, &desc) != ESP_OK) {
        return;
    }
    ESP_LOGI(TAG, "nuovo dispositivo %04x:%04x", desc->idVendor, desc->idProduct);
    atomic_store(&s_seen, USB_ID(desc->idVendor, desc->idProduct));
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}

/* Chiamata dal driver CDC quando arrivano dati dalla spada. */
static bool on_data(const uint8_t *data, size_t len, void *arg)
{
    size_t sent = pipe_write(s_rx_out, data, len);
    if (sent < len) {
        bridge_count_lost(len - sent);
        ESP_LOGW(TAG, "buffer verso il BLE pieno: %u byte persi", (unsigned)(len - sent));
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
        s_gone = true;            /* il task chiude l'handle */
        s_connected = false;
        bridge_state_changed();
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

/* Inoltra BLE -> spada finché la spada resta collegata. */
static void run_session(cdc_acm_dev_hdl_t dev, uint32_t id)
{
    const cdc_acm_line_coding_t coding = {
        .dwDTERate = 1152000, .bCharFormat = 0, .bParityType = 0, .bDataBits = 8,
    };
    uint8_t buf[TX_CHUNK];

    (void)cdc_acm_host_line_coding_set(dev, &coding);
    (void)cdc_acm_host_set_control_line_state(dev, ASSERT_DTR, ASSERT_RTS);
    ESP_LOGI(TAG, "spada collegata: %04x:%04x (DTR %d, RTS %d)", ID_VID(id), ID_PID(id),
             ASSERT_DTR, ASSERT_RTS);
    s_open_id = id;
    s_connected = true;
    bridge_state_changed();

    /* s_gone e non s_connected: l'evento può arrivare anche prima di s_connected = true */
    while (!s_gone) {
        size_t n = pipe_read(s_tx_in, buf, sizeof buf, pdMS_TO_TICKS(50));
        if (n > 0 && cdc_acm_host_data_tx_blocking(dev, buf, n, 1000) != ESP_OK) {
            ESP_LOGW(TAG, "scrittura verso la spada fallita");
        }
    }
    cdc_acm_host_close(dev);   /* va chiamata anche dopo l'evento di disconnessione */
    s_connected = false;
    s_open_id = 0;
    bridge_state_changed();
}

static void saber_task(void *arg)
{
    const cdc_acm_host_device_config_t dev_cfg = {
        .connection_timeout_ms = OPEN_TIMEOUT_MS,
        .out_buffer_size = 512,
        .event_cb = on_event,
        .data_cb = on_data,
        .user_arg = NULL,
    };
    uint32_t backoff = BACKOFF_MIN_MS;

    for (;;) {
        uint32_t id = atomic_load(&s_seen);
        if (id == 0) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
            continue;
        }
        if (!id_allowed(id)) {
            ESP_LOGW(TAG, "%04x:%04x non è una spada nota: ignorato", ID_VID(id), ID_PID(id));
            atomic_compare_exchange_strong(&s_seen, &id, 0);   /* tiene un eventuale dispositivo più nuovo */
            continue;
        }

        cdc_acm_dev_hdl_t dev = NULL;
        s_gone = false;
        esp_err_t err = cdc_acm_host_open(ID_VID(id), ID_PID(id), CONFIG_BRIDGE_CDC_INTERFACE,
                                          &dev_cfg, &dev);
        if (err != ESP_OK) {
            if (backoff == BACKOFF_MIN_MS) {
                ESP_LOGW(TAG, "apertura di %04x:%04x fallita: %s", ID_VID(id), ID_PID(id),
                         esp_err_to_name(err));
            }
            ESP_LOGD(TAG, "nuovo tentativo fra %lu ms", (unsigned long)backoff);
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(backoff));   /* un dispositivo nuovo sveglia prima */
            backoff = (backoff * 2 < BACKOFF_MAX_MS) ? backoff * 2 : BACKOFF_MAX_MS;
            continue;
        }
        backoff = BACKOFF_MIN_MS;

        /* Si azzera ora e non a fine sessione: se la spada ricompare subito dopo lo scollegamento,
         * on_new_dev la annota di nuovo e il ciclo la riapre. */
        uint32_t opened = id;
        atomic_compare_exchange_strong(&s_seen, &opened, 0);
        run_session(dev, id);
    }
}

void usb_saber_start(pipe_t *to_saber, pipe_t *to_ble)
{
    esp_log_level_set(TAG, CONFIG_BRIDGE_LOG_LEVEL);
    s_tx_in = to_saber;
    s_rx_out = to_ble;

    const usb_host_config_t host_cfg = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    ESP_ERROR_CHECK(usb_host_install(&host_cfg));
    xTaskCreate(usb_lib_task, "usb_lib", 4096, NULL, 10, NULL);
    xTaskCreate(saber_task, "saber", 4096, NULL, 5, &s_task);

    const cdc_acm_host_driver_config_t drv_cfg = {
        .driver_task_stack_size = 4096,
        .driver_task_priority = 10,
        .xCoreID = 0,
        .new_dev_cb = on_new_dev,
    };
    ESP_ERROR_CHECK(cdc_acm_host_install(&drv_cfg));
}
