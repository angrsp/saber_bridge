/* Lato spada della variante CONFIG_BRIDGE_FAKE_SABER: l'emulatore della NXT (nxt_emu.c) dietro gli
 * stessi pipe della spada vera. Dalla console USB della XIAO, "unplug" e "plug" simulano lo
 * scollegamento della spada. */
#include <stdio.h>
#include <string.h>
#include "bridge_log.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bridge.h"
#include "nxt_emu.h"
#include "saber.h"

static const char *TAG = "fake_saber";

#define FAKE_VID 0x0483   /* come la NXT */
#define FAKE_PID 0x5740
#define HEARTBEAT_MS 10000

static pipe_t *s_in;    /* BLE -> spada */
static pipe_t *s_out;   /* spada -> BLE */
static volatile bool s_connected = true;

bool saber_connected(void) { return s_connected; }

bool saber_ids(uint16_t *vid, uint16_t *pid)
{
    bool on = s_connected;
    *vid = on ? FAKE_VID : 0;
    *pid = on ? FAKE_PID : 0;
    return on;
}

static void emu_out(const char *data, size_t len)
{
    size_t sent = pipe_write(s_out, (const uint8_t *)data, len);
    if (sent < len) {
        bridge_count_lost(len - sent);
    }
}

static void fake_task(void *arg)
{
    uint8_t buf[64];
    TickType_t next_beat = xTaskGetTickCount() + pdMS_TO_TICKS(HEARTBEAT_MS);
    uint32_t beats = 0;

    for (;;) {
        size_t n = pipe_read(s_in, buf, sizeof buf, pdMS_TO_TICKS(50));
        if (!s_connected) {
            continue;
        }
        for (size_t i = 0; i < n; i++) {
            nxt_emu_feed(buf[i]);
        }
        /* ogni tanto una riga di log come quelle della spada in modalità debug, fra un comando e l'altro */
        if ((int32_t)(xTaskGetTickCount() - next_beat) >= 0) {
            next_beat += pdMS_TO_TICKS(HEARTBEAT_MS);
            if (nxt_emu_line_empty()) {
                char line[48];
                int len = snprintf(line, sizeof line, "#FakeSaber: alive %lu\n", (unsigned long)++beats);
                emu_out(line, (size_t)len);
            }
        }
    }
}

static void set_connected(bool on)
{
    if (s_connected == on) {
        return;
    }
    nxt_emu_clear_line();
    s_connected = on;
    ESP_LOGW(TAG, "spada %s (simulato)", on ? "collegata" : "scollegata");
    bridge_state_changed();
}

/* Comandi di debug dalla console USB della XIAO. */
static void console_task(void *arg)
{
    char line[32];
    size_t len = 0;
    printf("fake_saber: comandi \"unplug\", \"plug\"\n");
    for (;;) {
        uint8_t ch;
        if (usb_serial_jtag_read_bytes(&ch, 1, portMAX_DELAY) != 1) {
            continue;
        }
        if (ch != '\r' && ch != '\n') {
            if (len < sizeof line - 1) {
                line[len++] = (char)ch;
            }
            continue;
        }
        line[len] = '\0';
        len = 0;
        if (strcmp(line, "unplug") == 0) {
            set_connected(false);
        } else if (strcmp(line, "plug") == 0) {
            set_connected(true);
        } else if (line[0] != '\0') {
            printf("fake_saber: comando sconosciuto \"%s\" (unplug, plug)\n", line);
        }
    }
}

void saber_start(pipe_t *to_saber, pipe_t *to_ble)
{
    esp_log_level_set(TAG, CONFIG_BRIDGE_LOG_LEVEL);
    s_in = to_saber;
    s_out = to_ble;
    nxt_emu_init(emu_out);

    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&cfg));
    usb_serial_jtag_vfs_use_driver();   /* i log passano dal driver, che ora legge anche la console */

    xTaskCreate(fake_task, "fake_saber", 4096, NULL, 5, NULL);
    xTaskCreate(console_task, "fake_console", 3072, NULL, 2, NULL);
    ESP_LOGI(TAG, "variante fake: spada emulata %04x:%04x", FAKE_VID, FAKE_PID);
}
