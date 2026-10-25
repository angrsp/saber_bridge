#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "nxt_emu.h"

#define BANKS   8
#define MSG_MAX 256

#define STX 0x02
#define ETX 0x03
#define LF  0x0a

typedef struct {
    uint8_t r, g, b, w;
} color_t;

static nxt_emu_out_fn s_out;

static color_t s_main[BANKS];
static color_t s_clash[BANKS];
static color_t s_swing[BANKS];
static int s_bank;

static char s_msg[MSG_MAX + 1];
static size_t s_msg_len;

static void load_defaults(void)
{
    static const color_t main_defaults[BANKS] = {
        { 255, 0, 0, 0 }, { 0, 255, 0, 0 }, { 0, 0, 255, 0 }, { 255, 255, 0, 0 },
        { 0, 255, 255, 0 }, { 255, 0, 255, 0 }, { 255, 128, 0, 0 }, { 0, 0, 0, 255 },
    };
    for (int i = 0; i < BANKS; i++) {
        s_main[i] = main_defaults[i];
        s_clash[i] = (color_t){ 255, 255, 255, 255 };
        s_swing[i] = main_defaults[i];
    }
    s_bank = 0;
}

static void out_str(const char *s) { s_out(s, strlen(s)); }

static void reply(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void reply(const char *fmt, ...)
{
    char line[BANKS * 24 + 16];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line - 1, fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    if ((size_t)n > sizeof line - 2) {
        n = sizeof line - 2;
    }
    line[n++] = LF;
    s_out(line, (size_t)n);
}

/* Come CoreComms::stringToColorLed: senza separatori è esadecimale rrggbbww (con "#" o "0x" davanti
 * facoltativi), altrimenti decimale separato da virgole, punti e virgola o spazi. */
static color_t parse_color(const char *src)
{
    char s[MSG_MAX + 1];
    while (*src == ' ') {
        src++;
    }
    snprintf(s, sizeof s, "%s", src);
    for (size_t n = strlen(s); n > 0 && s[n - 1] == ' '; n--) {
        s[n - 1] = '\0';
    }

    color_t c = { 0 };
    if (!strpbrk(s, ",; ")) {
        const char *p = s;
        if (*p == '#') {
            p++;
        } else if (strncmp(p, "0x", 2) == 0) {
            p += 2;
        }
        uint32_t v = (uint32_t)strtoul(p, NULL, 16);
        c = (color_t){ (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
        return c;
    }
    uint8_t *f[4] = { &c.r, &c.g, &c.b, &c.w };
    char *p = s;
    for (int i = 0; i < 4 && p; i++) {
        *f[i] = (uint8_t)atoi(p);
        p = strpbrk(p, ",; ");
        if (p) {
            p++;
        }
    }
    return c;
}

static void format_color(char *buf, size_t size, color_t c, bool hex)
{
    if (hex) {
        snprintf(buf, size, "%02X%02X%02X%02X", c.r, c.g, c.b, c.w);
    } else {
        snprintf(buf, size, "%u,%u,%u,%u", c.r, c.g, c.b, c.w);
    }
}

static color_t *bank_table(char kind)
{
    switch (kind) {
    case 'C': case 'c': return s_main;
    case 'F': case 'f': return s_clash;
    case 'W': case 'w': return s_swing;
    default:            return NULL;
    }
}

static bool is_bank(char c) { return c >= '0' && c <= '7'; }

static void process(const char *m)
{
    size_t len = strlen(m);
    char c0 = m[0];
    char c1 = len > 1 ? m[1] : '\0';
    char c2 = len > 2 ? m[2] : '\0';
    char col[24];

    if (strcasecmp(m, "V") == 0) {
        /* Serial.println di Arduino termina con \r\n */
        out_str("Build: " NXT_EMU_BUILD "\r\nSerial Number: " NXT_EMU_SERIAL "\r\nFiles on memory:\n\n");
    } else if (c0 == '#') {
        out_str("#");                                   /* senza LF, come la spada */
    } else if (strcasecmp(m, "V?") == 0) {
        reply("V=%s", NXT_EMU_BUILD);
    } else if (strcmp(m, "S?") == 0) {
        reply("S=%s", NXT_EMU_SERIAL);
    } else if (strcasecmp(m, "WR?") == 0) {
        reply("OK, Write Ready");
    } else if (c2 == '=') {
        color_t *t = bank_table(c0);
        if (t && is_bank(c1)) {
            t[c1 - '0'] = parse_color(m + 3);
            reply("OK %s", m);                         /* eco del comando, non il colore normalizzato */
        }
    } else if (c2 == '?') {
        color_t *t = bank_table(c0);
        if (t && is_bank(c1)) {
            format_color(col, sizeof col, t[c1 - '0'], isupper((unsigned char)c0));
            reply("%c%c=%s", c0, c1, col);
        }
    } else if (len == 2 && c1 == '?' && bank_table(c0)) {
        color_t *t = bank_table(c0);
        bool hex = isupper((unsigned char)c0);
        char all[BANKS * 24];
        size_t n = 0;
        for (int i = 0; i < BANKS; i++) {
            format_color(col, sizeof col, t[i], hex);
            n += (size_t)snprintf(all + n, sizeof all - n, "%c%d=%s%s", c0, i, col,
                                  (i != BANKS - 1) ? "\n" : "");
        }
        reply("%s", all);
    } else if (strcasecmp(m, "P=0") == 0) {
        reply("OK, P=0");
    } else if (strncmp(m, "P=", 2) == 0 || strncmp(m, "p=", 2) == 0) {
        reply("OK, %s", m);
    } else if (strncmp(m, "B=", 2) == 0) {
        if (is_bank(c2)) {
            s_bank = c2 - '0';
            reply("OK %s", m);
        }
    } else if (strcmp(m, "B?") == 0) {
        reply("B=%d", s_bank);
    } else if (strcmp(m, "RESET") == 0) {
        load_defaults();
        reply("OK RESET");
    } else if (strcmp(m, "SAVE") == 0) {
        reply("OK SAVE");
    }
    /* tutto il resto: nessuna risposta */
}

void nxt_emu_init(nxt_emu_out_fn out)
{
    s_out = out;
    load_defaults();
    s_msg_len = 0;
}

void nxt_emu_feed(uint8_t b)
{
    if (b == STX && !(s_msg_len > 0 && s_msg[0] == '#')) {
        s_msg_len = 0;
    } else if (b == ETX || b == LF) {
        s_msg[s_msg_len] = '\0';
        process(s_msg);
        s_msg_len = 0;
    } else if (s_msg_len < MSG_MAX) {
        s_msg[s_msg_len++] = (char)b;
    }
}

void nxt_emu_clear_line(void)
{
    s_msg_len = 0;
}

bool nxt_emu_line_empty(void)
{
    return s_msg_len == 0;
}
