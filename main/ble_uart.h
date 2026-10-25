#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"

void ble_uart_start(StreamBufferHandle_t to_saber, StreamBufferHandle_t to_ble);
