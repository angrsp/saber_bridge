/* Prove dell'emulatore della NXT contro il comportamento di CoreComms.cpp (polaris-opencore). */
#include "check.h"
#include "nxt_emu.h"

static char g_out[1024];
static size_t g_len;

static void capture(const char *data, size_t len)
{
    memcpy(g_out + g_len, data, len);
    g_len += len;
    g_out[g_len] = '\0';
}

/* Manda i byte così come sono e ritorna quello che la spada risponde. */
static const char *send_raw(const char *bytes, size_t len)
{
    g_len = 0;
    g_out[0] = '\0';
    for (size_t i = 0; i < len; i++) {
        nxt_emu_feed((uint8_t)bytes[i]);
    }
    return g_out;
}

/* Manda una riga terminata da LF. */
static const char *cmd(const char *line)
{
    char buf[300];
    int n = snprintf(buf, sizeof buf, "%s\n", line);
    return send_raw(buf, (size_t)n);
}

int main(void)
{
    nxt_emu_init(capture);

    CHECK_STR(cmd("WR?"), "OK, Write Ready\n");
    CHECK_STR(cmd("wr?"), "OK, Write Ready\n");                 /* equalsIgnoreCase */
    CHECK_STR(cmd("V?"), "V=NXT_fake\n");
    CHECK_STR(cmd("S?"), "S=0000fa4e-00000000-00000001\n");
    CHECK_STR(cmd("s?"), "");                                   /* S? distingue le maiuscole */
    CHECK_STR(cmd("B?"), "B=0\n");

    /* comandi sconosciuti: nessuna risposta */
    CHECK_STR(cmd("XYZ"), "");
    CHECK_STR(cmd(""), "");
    CHECK_STR(cmd("C8=1,2,3,4"), "");
    CHECK_STR(cmd("S"), "");
    CHECK_STR(cmd("V?\r"), "");                                 /* il \r resta nel comando */

    /* righe "#": un solo carattere, senza LF */
    CHECK_STR(cmd("#legacy"), "#");

    /* scrittura: eco del comando */
    CHECK_STR(cmd("C0=255,0,0,0"), "OK C0=255,0,0,0\n");
    CHECK_STR(cmd("c1=00FF0000"), "OK c1=00FF0000\n");
    CHECK_STR(cmd("F2=#0A0B0C0D"), "OK F2=#0A0B0C0D\n");
    CHECK_STR(cmd("W3=1;2;3;4"), "OK W3=1;2;3;4\n");
    CHECK_STR(cmd("C4=0x11223344"), "OK C4=0x11223344\n");

    /* lettura di un bank: maiuscola = esadecimale, minuscola = decimale, con la lettera del comando */
    CHECK_STR(cmd("C0?"), "C0=FF000000\n");
    CHECK_STR(cmd("c0?"), "c0=255,0,0,0\n");
    CHECK_STR(cmd("c1?"), "c1=0,255,0,0\n");
    CHECK_STR(cmd("f2?"), "f2=10,11,12,13\n");
    CHECK_STR(cmd("W3?"), "W3=01020304\n");
    CHECK_STR(cmd("C4?"), "C4=11223344\n");
    CHECK_STR(cmd("c9?"), "");

    /* tutti i bank: 8 righe */
    CHECK_STR(cmd("c?"),
              "c0=255,0,0,0\nc1=0,255,0,0\nc2=0,0,255,0\nc3=255,255,0,0\n"
              "c4=17,34,51,68\nc5=255,0,255,0\nc6=255,128,0,0\nc7=0,0,0,255\n");
    CHECK_STR(cmd("F?"),
              "F0=FFFFFFFF\nF1=FFFFFFFF\nF2=0A0B0C0D\nF3=FFFFFFFF\n"
              "F4=FFFFFFFF\nF5=FFFFFFFF\nF6=FFFFFFFF\nF7=FFFFFFFF\n");
    CHECK(strncmp(cmd("w?"), "w0=255,0,0,0\n", 13) == 0);

    /* anteprima */
    CHECK_STR(cmd("P=0,0,255,0"), "OK, P=0,0,255,0\n");
    CHECK_STR(cmd("P=0"), "OK, P=0\n");
    CHECK_STR(cmd("p=0"), "OK, P=0\n");
    CHECK_STR(cmd("p=FF000000"), "OK, p=FF000000\n");

    /* bank attivo */
    CHECK_STR(cmd("B=5"), "OK B=5\n");
    CHECK_STR(cmd("B?"), "B=5\n");
    CHECK_STR(cmd("B=8"), "");

    CHECK_STR(cmd("SAVE"), "OK SAVE\n");
    CHECK_STR(cmd("save"), "");
    CHECK_STR(cmd("RESET"), "OK RESET\n");
    CHECK_STR(cmd("B?"), "B=0\n");
    CHECK_STR(cmd("c4?"), "c4=0,255,255,0\n");

    /* report per sviluppatori */
    CHECK(strncmp(cmd("V"), "Build: NXT_fake\r\n", 17) == 0);

    /* terminatori: ETX come LF; STX azzera la riga, ma non dopo un "#" */
    CHECK_STR(send_raw("B?\x03", 3), "B=0\n");
    CHECK_STR(send_raw("garbage\x02" "B?\n", 11), "B=0\n");
    CHECK_STR(send_raw("#x\x02y\n", 5), "#");

    /* più comandi nello stesso blocco, e un comando spezzato in due scritture */
    CHECK_STR(send_raw("V?\nB?\n", 6), "V=NXT_fake\nB=0\n");
    send_raw("WR", 2);
    CHECK(!nxt_emu_line_empty());
    CHECK_STR(send_raw("?\n", 2), "OK, Write Ready\n");
    CHECK(nxt_emu_line_empty());

    /* riga scartata allo scollegamento */
    send_raw("V", 1);
    nxt_emu_clear_line();
    CHECK_STR(cmd("B?"), "B=0\n");

    return check_report("test_nxt_emu");
}
