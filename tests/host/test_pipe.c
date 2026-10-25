/* Prove di pipe.c: scarto dei dati vecchi dopo pipe_flush, scrittura tutto o niente, giro dei contatori. */
#include "check.h"
#include "pipe.h"

/* Legge tutto il pipe a pezzi piccoli: così il segno di scarto può cadere a metà di una lettura. */
static size_t read_all(pipe_t *p, char *out, size_t max)
{
    size_t total = 0;
    uint8_t buf[4];
    while (p->sb->count > 0 && total < max) {
        size_t n = pipe_read(p, buf, sizeof buf, 0);
        memcpy(out + total, buf, n);
        total += n;
    }
    out[total] = '\0';
    return total;
}

static void write_str(pipe_t *p, const char *s)
{
    pipe_write(p, (const uint8_t *)s, strlen(s));
}

int main(void)
{
    pipe_t p;
    char got[64];

    pipe_init(&p, 16);
    write_str(&p, "abc");
    read_all(&p, got, sizeof got - 1);
    CHECK_STR(got, "abc");

    /* il flush scarta solo ciò che era già scritto */
    write_str(&p, "OLD");
    pipe_flush(&p);
    write_str(&p, "new");
    read_all(&p, got, sizeof got - 1);
    CHECK_STR(got, "new");

    /* segno di scarto a metà di una lettura da 4 byte */
    write_str(&p, "xxxxxx");
    pipe_flush(&p);
    write_str(&p, "ok");
    read_all(&p, got, sizeof got - 1);
    CHECK_STR(got, "ok");

    /* flush senza dati nuovi: non arriva niente */
    write_str(&p, "zz");
    pipe_flush(&p);
    CHECK(read_all(&p, got, sizeof got - 1) == 0);

    /* due flush di fila */
    write_str(&p, "a");
    pipe_flush(&p);
    write_str(&p, "b");
    pipe_flush(&p);
    write_str(&p, "c");
    read_all(&p, got, sizeof got - 1);
    CHECK_STR(got, "c");

    /* flush dopo una lettura parziale: il resto della generazione vecchia sparisce */
    write_str(&p, "12345678");
    uint8_t four[4];
    CHECK(pipe_read(&p, four, sizeof four, 0) == 4);
    pipe_flush(&p);
    write_str(&p, "Z");
    read_all(&p, got, sizeof got - 1);
    CHECK_STR(got, "Z");

    /* pipe_write scrive quanto ci sta, pipe_write_all tutto o niente */
    CHECK(pipe_write(&p, (const uint8_t *)"0123456789ABCDEFGH", 18) == 16);
    CHECK(!pipe_write_all(&p, (const uint8_t *)"x", 1));
    read_all(&p, got, sizeof got - 1);
    CHECK_STR(got, "0123456789ABCDEF");
    CHECK(pipe_write_all(&p, (const uint8_t *)"0123456789ABCDEF", 16));
    CHECK(!pipe_write_all(&p, (const uint8_t *)"x", 1));
    read_all(&p, got, sizeof got - 1);
    CHECK_STR(got, "0123456789ABCDEF");

    /* contatori vicini al giro di 2^32 */
    pipe_init(&p, 16);
    atomic_store(&p.written, 0xfffffffe);
    atomic_store(&p.consumed, 0xfffffffe);
    atomic_store(&p.discard_to, 0xfffffffe);
    write_str(&p, "OLD");
    pipe_flush(&p);
    write_str(&p, "wrap");
    read_all(&p, got, sizeof got - 1);
    CHECK_STR(got, "wrap");

    return check_report("test_pipe");
}
