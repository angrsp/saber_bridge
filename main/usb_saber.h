#pragma once
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"

/* to_saber: byte BLE -> spada (qui si legge). to_ble: byte spada -> BLE (qui si scrive). */
void usb_saber_start(StreamBufferHandle_t to_saber, StreamBufferHandle_t to_ble);
bool usb_saber_connected(void);
