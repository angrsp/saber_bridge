#pragma once
#include <stdbool.h>
#include "pipe.h"

/* Lato spada del bridge. Lo implementa usb_saber.c (host USB CDC/ACM verso la spada vera) oppure,
 * con CONFIG_BRIDGE_FAKE_SABER, fake_saber.c (emulatore della NXT, senza host USB).
 * to_saber: byte BLE -> spada (qui si legge). to_ble: byte spada -> BLE (qui si scrive). */
void saber_start(pipe_t *to_saber, pipe_t *to_ble);
bool saber_connected(void);
/* VID e PID della spada collegata; false e zeri se non c'è. */
bool saber_ids(uint16_t *vid, uint16_t *pid);
