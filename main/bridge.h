#pragma once
#include <stdint.h>

/* Crea i due pipe e avvia USB, BLE e LED. */
void bridge_start(void);

/* Da chiamare a ogni cambio di stato USB (spada collegata/scollegata) o BLE (client
 * connesso/disconnesso): svuota entrambi i pipe, così nessun dato vecchio viene inoltrato. */
void bridge_state_changed(void);

/* Byte persi nella direzione spada -> BLE (buffer pieno o notifica fallita). */
void bridge_count_lost(uint32_t n);
/* Ritorna i byte persi dall'ultima chiamata e azzera il contatore. */
uint32_t bridge_take_lost(void);
