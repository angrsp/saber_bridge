#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Prepara l'ADC. Ritorna false se la lettura della batteria è disattivata o non disponibile. */
bool battery_init(void);

/* Tensione della batteria in mV, media di più campioni; 0 = non misurata. */
uint16_t battery_read_mv(void);

/* Batteria bassa per la lettura mv, con isteresi: sotto la soglia si accende, si spegne 100 mV sopra. */
bool battery_is_low(uint16_t mv);
