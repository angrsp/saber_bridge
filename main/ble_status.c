#include <stdlib.h>
#include "bridge_log.h"
#include "esp_app_desc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "services/dis/ble_svc_dis.h"
#include "battery.h"
#include "bridge.h"
#include "usb_saber.h"
#include "ble_status.h"

static const char *TAG = "ble_status";

/* Payload della caratteristica di stato (contratto, 3.3): 8 byte little-endian. */
#define STATUS_FORMAT     1
#define STATUS_LEN        8
#define FLAG_SABER        0x01
#define FLAG_LOST         0x02
#define FLAG_BATTERY_LOW  0x04

#define BATTERY_PERIOD_MS 30000

#define MODEL_NUMBER "SaberBridge-S3"

static uint16_t s_handle;
static volatile uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static volatile bool s_notify_en;
static TaskHandle_t s_task;

static bool s_battery;               /* lettura della batteria attiva */
static volatile uint16_t s_battery_mv;
static volatile bool s_battery_low;

/* 7A6F0001-3C1D-4B8E-9F2A-6D5E4C3B2A10 e 7A6F0002-..., in little-endian */
static const ble_uuid128_t uuid_svc = BLE_UUID128_INIT(
    0x10, 0x2A, 0x3B, 0x4C, 0x5E, 0x6D, 0x2A, 0x9F, 0x8E, 0x4B, 0x1D, 0x3C, 0x01, 0x00, 0x6F, 0x7A);
static const ble_uuid128_t uuid_status = BLE_UUID128_INIT(
    0x10, 0x2A, 0x3B, 0x4C, 0x5E, 0x6D, 0x2A, 0x9F, 0x8E, 0x4B, 0x1D, 0x3C, 0x02, 0x00, 0x6F, 0x7A);

/* put_le16 viene da NimBLE (os/endian.h). Riempie il payload e ritorna i byte persi che riporta: vanno confermati con bridge_ack_lost()
 * solo se il payload arriva al client. */
static uint32_t build_status(uint8_t out[STATUS_LEN])
{
    uint16_t vid = 0, pid = 0;
    bool saber = usb_saber_ids(&vid, &pid);
    uint32_t lost = bridge_peek_lost();

    out[0] = STATUS_FORMAT;
    out[1] = (saber ? FLAG_SABER : 0) | (lost ? FLAG_LOST : 0) | (s_battery_low ? FLAG_BATTERY_LOW : 0);
    put_le16(out + 2, vid);
    put_le16(out + 4, pid);
    put_le16(out + 6, s_battery_mv);
    return lost;
}

static int gatt_status_cb(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    uint8_t buf[STATUS_LEN];
    uint32_t lost = build_status(buf);
    if (os_mbuf_append(ctxt->om, buf, sizeof buf) != 0) {
        return BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    bridge_ack_lost(lost);
    return 0;
}

static const struct ble_gatt_svc_def gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &uuid_svc.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = &uuid_status.u, .access_cb = gatt_status_cb, .val_handle = &s_handle,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY },
            { 0 },
        },
    },
    { 0 },
};

void ble_status_register(void)
{
    int rc = ble_gatts_count_cfg(gatt_svcs);
    if (rc == 0) {
        rc = ble_gatts_add_svcs(gatt_svcs);
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "servizio di stato non registrato: %d", rc);
        abort();
    }

    ble_svc_dis_init();
    ble_svc_dis_firmware_revision_set(esp_app_get_description()->version);
    ble_svc_dis_model_number_set(MODEL_NUMBER);
}

void ble_status_on_gap_event(const struct ble_gap_event *ev)
{
    switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (ev->connect.status == 0) {
            s_conn = ev->connect.conn_handle;
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_notify_en = false;
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (ev->subscribe.attr_handle == s_handle) {
            s_notify_en = ev->subscribe.cur_notify;
            if (s_notify_en) {
                ble_status_changed();   /* stato iniziale appena il client si iscrive */
            }
        }
        break;
    default:
        break;
    }
}

void ble_status_changed(void)
{
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}

static void send_status(void)
{
    uint16_t conn = s_conn;
    if (conn == BLE_HS_CONN_HANDLE_NONE || !s_notify_en) {
        return;
    }
    uint8_t buf[STATUS_LEN];
    uint32_t lost = build_status(buf);
    struct os_mbuf *om = ble_hs_mbuf_from_flat(buf, sizeof buf);
    if (!om) {
        return;
    }
    int rc = ble_gatts_notify_custom(conn, s_handle, om);   /* consuma l'mbuf in ogni caso */
    if (rc == 0) {
        bridge_ack_lost(lost);
        ESP_LOGD(TAG, "stato: flag 0x%02x, batteria %u mV", buf[1], s_battery_mv);
    } else {
        ESP_LOGW(TAG, "notifica di stato fallita: %d", rc);
    }
}

static void sample_battery(void)
{
    uint16_t mv = battery_read_mv();
    s_battery_mv = mv;
    s_battery_low = battery_is_low(mv);
}

/* Notifica a ogni cambio di stato e, con la batteria attiva, ogni 30 s. */
static void status_task(void *arg)
{
    TickType_t next_battery = xTaskGetTickCount();
    for (;;) {
        TickType_t wait = portMAX_DELAY;
        if (s_battery) {
            TickType_t now = xTaskGetTickCount();
            wait = ((int32_t)(next_battery - now) > 0) ? next_battery - now : 0;
        }
        bool changed = ulTaskNotifyTake(pdTRUE, wait) > 0;

        if (s_battery && (int32_t)(xTaskGetTickCount() - next_battery) >= 0) {
            sample_battery();
            next_battery += pdMS_TO_TICKS(BATTERY_PERIOD_MS);
            changed = true;
        }
        if (changed) {
            send_status();
        }
    }
}

void ble_status_start(void)
{
    esp_log_level_set(TAG, CONFIG_BRIDGE_LOG_LEVEL);
    s_battery = battery_init();
    xTaskCreate(status_task, "ble_status", 3072, NULL, 4, &s_task);
}
