#pragma once
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"

/* Stream buffer che si può svuotare da qualsiasi task senza violare la regola di FreeRTOS
 * "un solo scrittore e un solo lettore".
 *
 * Lo scrittore conta i byte scritti, il lettore quelli letti. pipe_flush() non tocca il buffer:
 * segna fin dove arrivano i dati vecchi (il contatore dei byte scritti in quel momento), e il
 * lettore scarta da sé tutto ciò che sta prima del segno. È un contatore di generazione espresso
 * in posizioni di byte: ogni cambio di stato apre una generazione nuova. */
typedef struct {
    StreamBufferHandle_t sb;
    _Atomic uint32_t written;      /* solo lo scrittore lo incrementa */
    _Atomic uint32_t consumed;     /* solo il lettore lo incrementa */
    _Atomic uint32_t discard_to;   /* i byte prima di questa posizione sono vecchi */
} pipe_t;

void pipe_init(pipe_t *p, size_t size);

/* Scrittore. pipe_write scrive quanto ci sta e ritorna i byte scritti;
 * pipe_write_all scrive tutto o niente. */
size_t pipe_write(pipe_t *p, const uint8_t *data, size_t len);
bool pipe_write_all(pipe_t *p, const uint8_t *data, size_t len);

/* Lettore. Ritorna i byte validi messi in buf; 0 per timeout o se erano tutti vecchi. */
size_t pipe_read(pipe_t *p, uint8_t *buf, size_t max, TickType_t timeout);

/* Qualsiasi task: i dati scritti fino a ora non verranno consegnati. */
void pipe_flush(pipe_t *p);
