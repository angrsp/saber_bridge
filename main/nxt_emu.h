#pragma once
/* Emulatore del protocollo seriale della Anima NXT, in C puro (si prova anche sul PC: tests/host).
 *
 * Ricalca CoreComms::processIncomingMessage di polaris-opencore (libraries/CoreComms/CoreComms.cpp):
 * stesso ordine dei controlli, confronto esatto (un "\r" in fondo rende il comando sconosciuto),
 * risposte terminate da "\n", silenzio sui comandi sconosciuti. Emula solo i comandi dei colori.
 * Le risposte vanno allineate a quelle reali della NXT registrate in F0 (piano, sezione 6). */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NXT_EMU_BUILD  "NXT_fake"
#define NXT_EMU_SERIAL "0000fa4e-00000000-00000001"

/* Riceve i byte che la spada manderebbe sulla seriale. */
typedef void (*nxt_emu_out_fn)(const char *data, size_t len);

/* Colori e bank ai valori di default, riga in arrivo vuota. */
void nxt_emu_init(nxt_emu_out_fn out);

/* Un byte dalla seriale: STX azzera la riga (tranne dopo un "#"), ETX o LF la eseguono. */
void nxt_emu_feed(uint8_t b);

/* Scarta la riga in arrivo (spada scollegata e ricollegata). */
void nxt_emu_clear_line(void);

/* Nessuna riga a metà: si può scrivere una riga di log senza spezzare un comando. */
bool nxt_emu_line_empty(void);
