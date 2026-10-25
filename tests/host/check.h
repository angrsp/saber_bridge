#pragma once
#include <stdio.h>
#include <string.h>

static int g_failures;
static int g_checks;

#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { g_failures++; printf("FALLITO %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

#define CHECK_STR(got, want) do { \
    g_checks++; \
    if (strcmp((got), (want)) != 0) { \
        g_failures++; \
        printf("FALLITO %s:%d\n  atteso:   \"%s\"\n  ottenuto: \"%s\"\n", __FILE__, __LINE__, (want), (got)); \
    } \
} while (0)

static int check_report(const char *name)
{
    printf("%s: %d controlli, %d falliti\n", name, g_checks, g_failures);
    return g_failures ? 1 : 0;
}
