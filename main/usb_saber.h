#pragma once
#include <stdbool.h>
#include "pipe.h"

/* to_saber: byte BLE -> spada (qui si legge). to_ble: byte spada -> BLE (qui si scrive). */
void usb_saber_start(pipe_t *to_saber, pipe_t *to_ble);
bool usb_saber_connected(void);
