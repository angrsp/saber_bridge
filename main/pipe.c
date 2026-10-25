#include <string.h>
#include "pipe.h"

void pipe_init(pipe_t *p, size_t size)
{
    p->sb = xStreamBufferCreate(size, 1);
    configASSERT(p->sb);
    atomic_init(&p->written, 0);
    atomic_init(&p->consumed, 0);
    atomic_init(&p->discard_to, 0);
}

static size_t write_n(pipe_t *p, const uint8_t *data, size_t n)
{
    /* Il contatore sale prima dell'invio: un flush concorrente include questi byte o li lascia
     * tutti alla generazione nuova, mai a metà. Con un solo scrittore lo spazio libero può solo
     * crescere, quindi l'invio non è mai parziale. */
    atomic_fetch_add(&p->written, (uint32_t)n);
    size_t sent = xStreamBufferSend(p->sb, data, n, 0);
    configASSERT(sent == n);
    return sent;
}

size_t pipe_write(pipe_t *p, const uint8_t *data, size_t len)
{
    size_t space = xStreamBufferSpacesAvailable(p->sb);
    size_t n = (len < space) ? len : space;
    return (n > 0) ? write_n(p, data, n) : 0;
}

bool pipe_write_all(pipe_t *p, const uint8_t *data, size_t len)
{
    if (xStreamBufferSpacesAvailable(p->sb) < len) {
        return false;
    }
    write_n(p, data, len);
    return true;
}

size_t pipe_read(pipe_t *p, uint8_t *buf, size_t max, TickType_t timeout)
{
    size_t n = xStreamBufferReceive(p->sb, buf, max, timeout);
    if (n == 0) {
        return 0;
    }
    uint32_t start = atomic_load(&p->consumed);
    atomic_store(&p->consumed, start + (uint32_t)n);

    /* differenza con segno: i contatori possono fare il giro */
    int32_t stale = (int32_t)(atomic_load(&p->discard_to) - start);
    if (stale <= 0) {
        return n;
    }
    if ((size_t)stale >= n) {
        return 0;
    }
    memmove(buf, buf + stale, n - (size_t)stale);
    return n - (size_t)stale;
}

void pipe_flush(pipe_t *p)
{
    atomic_store(&p->discard_to, atomic_load(&p->written));
}
