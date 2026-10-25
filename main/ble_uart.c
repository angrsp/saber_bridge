#include <stdlib.h>
#include <string.h>
#include "bridge_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "bridge.h"
#include "usb_saber.h"
#include "ble_uart.h"
#include "ble_status.h"

/* Definita in NimBLE (store/config) senza header pubblico, come nell'esempio bleprph. */
void ble_store_config_init(void);

static const char *TAG = "ble_uart";

/* Il nome va nella scan response: 31 byte meno 2 di intestazione del campo. */
_Static_assert(sizeof(CONFIG_BRIDGE_BLE_NAME) - 1 <= 29, "CONFIG_BRIDGE_BLE_NAME: al massimo 29 caratteri");

#define MAX_WRITE 244   /* MTU preferita 247 - 3; niente long write */

/* Errori ATT applicativi della write su RX (contratto, 3.2). */
#define ATT_ERR_SABER_ABSENT 0x80
#define ATT_ERR_BUFFER_FULL  0x81

/* Intervallo di connessione richiesto: 15-30 ms (unità da 1,25 ms), timeout 4 s (unità da 10 ms). */
#define CONN_ITVL_MIN    12
#define CONN_ITVL_MAX    24
#define CONN_SUPERVISION 400

static pipe_t *s_to_saber;
static pipe_t *s_to_ble;

static uint8_t s_own_addr_type;
static uint16_t s_tx_handle;
static volatile uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static volatile bool s_notify_en;

bool ble_uart_connected(void) { return s_conn != BLE_HS_CONN_HANDLE_NONE; }

/* Nordic UART Service. BLE_UUID128_INIT vuole i byte in little-endian. */
static const ble_uuid128_t uuid_svc = BLE_UUID128_INIT(
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0, 0x93, 0xF3, 0xA3, 0xB5, 0x01, 0x00, 0x40, 0x6E);
static const ble_uuid128_t uuid_rx = BLE_UUID128_INIT(
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0, 0x93, 0xF3, 0xA3, 0xB5, 0x02, 0x00, 0x40, 0x6E);
static const ble_uuid128_t uuid_tx = BLE_UUID128_INIT(
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0, 0x93, 0xF3, 0xA3, 0xB5, 0x03, 0x00, 0x40, 0x6E);

static void advertise(void);

/* app -> bridge: i byte scritti vanno nel pipe verso la spada, tutti o nessuno */
static int gatt_rx_cb(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    uint8_t tmp[MAX_WRITE];
    uint16_t len = 0;
    if (OS_MBUF_PKTLEN(ctxt->om) > sizeof tmp) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    if (ble_hs_mbuf_to_flat(ctxt->om, tmp, sizeof tmp, &len) != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    if (!usb_saber_connected()) {
        ESP_LOGD(TAG, "write di %u byte rifiutata: spada non collegata", len);
        return ATT_ERR_SABER_ABSENT;
    }
    if (!pipe_write_all(s_to_saber, tmp, len)) {
        ESP_LOGW(TAG, "write di %u byte rifiutata: buffer verso la spada pieno", len);
        return ATT_ERR_BUFFER_FULL;
    }
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

static void request_conn_params(uint16_t conn)
{
    const struct ble_gap_upd_params params = {
        .itvl_min = CONN_ITVL_MIN,
        .itvl_max = CONN_ITVL_MAX,
        .latency = 0,
        .supervision_timeout = CONN_SUPERVISION,
    };
    int rc = ble_gap_update_params(conn, &params);
    if (rc != 0) {
        ESP_LOGW(TAG, "richiesta dei parametri di connessione fallita: %d", rc);
    }
}

static int gap_event(struct ble_gap_event *ev, void *arg)
{
    ble_status_on_gap_event(ev);   /* prima del flush: la notifica di stato usa la connessione nuova */

    switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (ev->connect.status == 0) {
            s_conn = ev->connect.conn_handle;
            ESP_LOGI(TAG, "client connesso");
            bridge_state_changed();
            request_conn_params(ev->connect.conn_handle);
        } else {
            advertise();
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_notify_en = false;
        ESP_LOGI(TAG, "client disconnesso: 0x%x", ev->disconnect.reason);
        bridge_state_changed();
        advertise();
        break;
    case BLE_GAP_EVENT_CONN_UPDATE:
        if (ev->conn_update.status == 0) {
            struct ble_gap_conn_desc desc;
            if (ble_gap_conn_find(ev->conn_update.conn_handle, &desc) == 0) {
                ESP_LOGI(TAG, "intervallo di connessione %u.%02u ms",
                         desc.conn_itvl * 125 / 100, desc.conn_itvl * 125 % 100);
            }
        }
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

/* ble_gatts_notify_custom consuma l'mbuf anche in caso di errore: a ogni tentativo se ne crea uno nuovo.
 * Ritorna false se i byte non sono stati consegnati a un client in ascolto. */
static bool send_notify(const uint8_t *data, size_t len)
{
    for (int tries = 0; tries < 20; tries++) {
        if (s_conn == BLE_HS_CONN_HANDLE_NONE || !s_notify_en) {
            return true;                             /* nessun client in ascolto: si scarta, non è una perdita */
        }
        struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
        if (om) {
            int rc = ble_gatts_notify_custom(s_conn, s_tx_handle, om);
            if (rc == 0) {
                return true;
            }
            if (rc != BLE_HS_ENOMEM) {
                ESP_LOGW(TAG, "notify rc=%d", rc);
                return false;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return false;
}

/* spada -> app: legge dal pipe e manda notifiche da (MTU - 3) byte */
static void notify_task(void *arg)
{
    uint8_t buf[256];
    for (;;) {
        size_t n = pipe_read(s_to_ble, buf, sizeof buf, portMAX_DELAY);
        size_t off = 0;
        while (off < n) {
            uint16_t conn = s_conn;
            uint16_t mtu = (conn != BLE_HS_CONN_HANDLE_NONE) ? ble_att_mtu(conn) : 23;
            size_t slice = (mtu > 3) ? (size_t)(mtu - 3) : 20;
            size_t chunk = (n - off < slice) ? (n - off) : slice;
            if (!send_notify(buf + off, chunk)) {
                bridge_count_lost(chunk);
                ESP_LOGW(TAG, "notifica fallita: %u byte persi", (unsigned)chunk);
            }
            off += chunk;
        }
    }
}

static void host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void ble_uart_start(pipe_t *to_saber, pipe_t *to_ble)
{
    esp_log_level_set(TAG, CONFIG_BRIDGE_LOG_LEVEL);
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
    ble_status_register();
    ble_svc_gap_device_name_set(CONFIG_BRIDGE_BLE_NAME);
    ble_att_set_preferred_mtu(247);
    ble_store_config_init();

    ble_status_start();
    xTaskCreate(notify_task, "ble_notify", 4096, NULL, 5, NULL);
    nimble_port_freertos_init(host_task);
}
