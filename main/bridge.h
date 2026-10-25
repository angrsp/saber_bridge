#pragma once
#include <stdint.h>

/* Crea i due pipe e avvia USB, BLE e LED. */
void bridge_start(void);

/* Da chiamare a ogni cambio di stato USB (spada collegata/scollegata) o BLE (client
 * connesso/disconnesso): svuota entrambi i pipe, così nessun dato vecchio viene inoltrato,
 * e notifica il nuovo stato. */
void bridge_state_changed(void);

/* Byte persi nella direzione spada -> BLE (buffer pieno o notifica fallita). */
void bridge_count_lost(uint32_t n);
/* Byte persi non ancora riportati al client. */
uint32_t bridge_peek_lost(void);
/* Il client ha ricevuto uno stato che riportava n byte persi: si tolgono dal contatore. */
void bridge_ack_lost(uint32_t n);
