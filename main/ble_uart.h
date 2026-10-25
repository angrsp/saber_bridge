#pragma once
#include <stdbool.h>
#include "pipe.h"

void ble_uart_start(pipe_t *to_saber, pipe_t *to_ble);
bool ble_uart_connected(void);
