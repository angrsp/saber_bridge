#pragma once
#include <stdbool.h>
#include "pipe.h"

/* to_saber: byte BLE -> spada (qui si legge). to_ble: byte spada -> BLE (qui si scrive). */
void usb_saber_start(pipe_t *to_saber, pipe_t *to_ble);
bool usb_saber_connected(void);
/* VID e PID della spada collegata; false e zeri se non c'è. */
bool usb_saber_ids(uint16_t *vid, uint16_t *pid);
