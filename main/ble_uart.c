#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "ble_uart.h"

/* Definita in NimBLE (store/config) senza header pubblico, come nell'esempio bleprph. */
void ble_store_config_init(void);

static const char *TAG = "ble_uart";
#define DEVICE_NAME "SaberBridge"

static StreamBufferHandle_t s_to_saber;
static StreamBufferHandle_t s_to_ble;

static uint8_t s_own_addr_type;
static uint16_t s_tx_handle;
static volatile uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static volatile bool s_notify_en;

/* Nordic UART Service. BLE_UUID128_INIT vuole i byte in little-endian. */
static const ble_uuid128_t uuid_svc = BLE_UUID128_INIT(
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0, 0x93, 0xF3, 0xA3, 0xB5, 0x01, 0x00, 0x40, 0x6E);
static const ble_uuid128_t uuid_rx = BLE_UUID128_INIT(
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0, 0x93, 0xF3, 0xA3, 0xB5, 0x02, 0x00, 0x40, 0x6E);
static const ble_uuid128_t uuid_tx = BLE_UUID128_INIT(
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0, 0x93, 0xF3, 0xA3, 0xB5, 0x03, 0x00, 0x40, 0x6E);

static void advertise(void);

/* app -> bridge: i byte scritti vanno nello stream buffer verso la spada */
static int gatt_rx_cb(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    uint8_t tmp[256];
    uint16_t len = 0;
    if (OS_MBUF_PKTLEN(ctxt->om) > sizeof tmp) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    if (ble_hs_mbuf_to_flat(ctxt->om, tmp, sizeof tmp, &len) != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    xStreamBufferSend(s_to_saber, tmp, len, 0);   /* se pieno, i byte si perdono */
    return 0;
}

/* La caratteristica TX è solo notify: serve comunque una access_cb. */
static int gatt_tx_cb(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    return 0;
}

static const struct ble_gatt_svc_def gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &uuid_svc.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = &uuid_rx.u, .access_cb = gatt_rx_cb,
              .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP },
            { .uuid = &uuid_tx.u, .access_cb = gatt_tx_cb, .val_handle = &s_tx_handle,
              .flags = BLE_GATT_CHR_F_NOTIFY },
            { 0 },
        },
    },
    { 0 },
};

static int gap_event(struct ble_gap_event *ev, void *arg)
{
    switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (ev->connect.status == 0) {
            s_conn = ev->connect.conn_handle;
            ESP_LOGI(TAG, "client connesso");
        } else {
            advertise();
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_notify_en = false;
        ESP_LOGI(TAG, "client disconnesso");
        advertise();
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (ev->subscribe.attr_handle == s_tx_handle) {
            s_notify_en = ev->subscribe.cur_notify;
        }
        break;
    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU = %d", ev->mtu.value);
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        break;
    default:
        break;
    }
    return 0;
}

static void advertise(void)
{
    struct ble_hs_adv_fields fields = { 0 };
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = (ble_uuid128_t *)&uuid_svc;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;
    ble_gap_adv_set_fields(&fields);

    struct ble_hs_adv_fields rsp = { 0 };           /* il nome va nella scan response */
    const char *name = ble_svc_gap_device_name();
    rsp.name = (uint8_t *)name;
    rsp.name_len = strlen(name);
    rsp.name_is_complete = 1;
    ble_gap_adv_rsp_set_fields(&rsp);

    struct ble_gap_adv_params params = { 0 };
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    int rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &params, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "advertising non avviato: %d", rc);
    }
}

static void on_reset(int reason)
{
    ESP_LOGE(TAG, "reset dello stack BLE: %d", reason);
}

static void on_sync(void)
{
    ble_hs_id_infer_auto(0, &s_own_addr_type);
    advertise();
}

static void send_notify(const uint8_t *data, size_t len)
{
    for (int tries = 0; tries < 20; tries++) {
        if (s_conn == BLE_HS_CONN_HANDLE_NONE || !s_notify_en) {
            return;                                  /* nessun client in ascolto: scarta */
        }
        struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
        if (om) {
            int rc = ble_gatts_notify_custom(s_conn, s_tx_handle, om);
            if (rc == 0) {
                return;
            }
            if (rc != BLE_HS_ENOMEM) {
                ESP_LOGW(TAG, "notify rc=%d", rc);
                return;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* spada -> app: legge dallo stream buffer e manda notifiche da (MTU - 3) byte */
static void notify_task(void *arg)
{
    uint8_t buf[256];
    for (;;) {
        size_t n = xStreamBufferReceive(s_to_ble, buf, sizeof buf, portMAX_DELAY);
        size_t off = 0;
        while (off < n) {
            uint16_t mtu = (s_conn != BLE_HS_CONN_HANDLE_NONE) ? ble_att_mtu(s_conn) : 23;
            size_t slice = (mtu > 3) ? (size_t)(mtu - 3) : 20;
            size_t chunk = (n - off < slice) ? (n - off) : slice;
            send_notify(buf + off, chunk);
            off += chunk;
        }
    }
}

static void host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void ble_uart_start(StreamBufferHandle_t to_saber, StreamBufferHandle_t to_ble)
{
    s_to_saber = to_saber;
    s_to_ble = to_ble;

    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_gatts_count_cfg(gatt_svcs);
    if (rc == 0) {
        rc = ble_gatts_add_svcs(gatt_svcs);
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "servizi GATT non registrati: %d", rc);
        abort();
    }
    ble_svc_gap_device_name_set(DEVICE_NAME);
    ble_att_set_preferred_mtu(247);

    ble_store_config_init();

    xTaskCreate(notify_task, "ble_notify", 4096, NULL, 5, NULL);
    nimble_port_freertos_init(host_task);
}
