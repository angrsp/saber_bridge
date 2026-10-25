/* L'emulatore della NXT su una pseudo-seriale (Linux): serve a provare tools/f0_probe.py senza spada.
 * Stampa il percorso della pty e risponde finché non viene terminato. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "nxt_emu.h"

static int g_master;

static void out(const char *data, size_t len)
{
    while (len > 0) {
        ssize_t n = write(g_master, data, len);
        if (n <= 0) {
            return;
        }
        data += n;
        len -= (size_t)n;
    }
}

int main(void)
{
    g_master = posix_openpt(O_RDWR | O_NOCTTY);
    if (g_master < 0 || grantpt(g_master) != 0 || unlockpt(g_master) != 0) {
        perror("pty");
        return 1;
    }
    printf("%s\n", ptsname(g_master));
    fflush(stdout);

    nxt_emu_init(out);
    for (;;) {
        unsigned char buf[256];
        ssize_t n = read(g_master, buf, sizeof buf);
        if (n < 0 && errno == EIO) {   /* nessuno ha la pty aperta: si aspetta che venga riaperta */
            usleep(50000);
            continue;
        }
        if (n <= 0) {
            break;
        }
        for (ssize_t i = 0; i < n; i++) {
            nxt_emu_feed(buf[i]);
        }
    }
    return 0;
}
