#include <stdatomic.h>
#include "bridge_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pipe.h"
#include "saber.h"
#include "ble_uart.h"
#include "ble_status.h"
#include "bridge.h"

static const char *TAG = "bridge";

#define BUF_SIZE 2048

/* to_saber: scrive la callback GATT, legge il task USB.
 * to_ble:   scrive la callback dati CDC, legge il task di notifica BLE. */
static pipe_t s_to_saber;
static pipe_t s_to_ble;
static _Atomic uint32_t s_lost;

void bridge_state_changed(void)
{
    pipe_flush(&s_to_saber);
    pipe_flush(&s_to_ble);
    ble_status_changed();
}

void bridge_count_lost(uint32_t n)
{
    if (atomic_fetch_add(&s_lost, n) == 0) {
        ble_status_changed();   /* il flag "byte persi" passa a 1 */
    }
}

uint32_t bridge_peek_lost(void)
{
    return atomic_load(&s_lost);
}

void bridge_ack_lost(uint32_t n)
{
    atomic_fetch_sub(&s_lost, n);
}

#if CONFIG_BRIDGE_LED_GPIO >= 0

#define LED_TICK_MS 50

#ifdef CONFIG_BRIDGE_LED_ACTIVE_LOW
#define LED_ACTIVE_LOW true
#else
#define LED_ACTIVE_LOW false
#endif

static void led_set(bool on)
{
    gpio_set_level(CONFIG_BRIDGE_LED_GPIO, on != LED_ACTIVE_LOW);
}

/* Lampeggio lento: advertising, nessun client.
 * Lampeggio veloce: client connesso, spada assente.
 * Fisso: client connesso e spada collegata. */
static void led_task(void *arg)
{
    uint32_t tick = 0;
    for (;;) {
        bool on;
        if (!ble_uart_connected()) {
            on = (tick / (500 / LED_TICK_MS)) % 2 == 0;
        } else if (!saber_connected()) {
            on = (tick / (100 / LED_TICK_MS)) % 2 == 0;
        } else {
            on = true;
        }
        led_set(on);
        tick++;
        vTaskDelay(pdMS_TO_TICKS(LED_TICK_MS));
    }
}

static void led_start(void)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << CONFIG_BRIDGE_LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
    led_set(false);
    xTaskCreate(led_task, "led", 2048, NULL, 1, NULL);
}

#else

static void led_start(void) { }

#endif

void bridge_start(void)
{
    esp_log_level_set(TAG, CONFIG_BRIDGE_LOG_LEVEL);

    pipe_init(&s_to_saber, BUF_SIZE);
    pipe_init(&s_to_ble, BUF_SIZE);

    saber_start(&s_to_saber, &s_to_ble);
    ble_uart_start(&s_to_saber, &s_to_ble);
    led_start();
    ESP_LOGI(TAG, "bridge avviato");
}
