#pragma once
/* Stream buffer finto: un buffer circolare senza attese (a un solo thread il timeout non serve). */
#include <stddef.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"

typedef struct {
    unsigned char *data;
    size_t size, head, count;
} fake_sb_t;
typedef fake_sb_t *StreamBufferHandle_t;

static inline StreamBufferHandle_t xStreamBufferCreate(size_t size, size_t trigger)
{
    (void)trigger;
    fake_sb_t *sb = calloc(1, sizeof *sb);
    sb->data = malloc(size);
    sb->size = size;
    return sb;
}

static inline size_t xStreamBufferSpacesAvailable(StreamBufferHandle_t sb)
{
    return sb->size - sb->count;
}

static inline size_t xStreamBufferSend(StreamBufferHandle_t sb, const void *src, size_t len, TickType_t t)
{
    (void)t;
    const unsigned char *p = src;
    size_t n = 0;
    while (n < len && sb->count < sb->size) {
        sb->data[(sb->head + sb->count) % sb->size] = p[n++];
        sb->count++;
    }
    return n;
}

static inline size_t xStreamBufferReceive(StreamBufferHandle_t sb, void *dst, size_t max, TickType_t t)
{
    (void)t;
    unsigned char *p = dst;
    size_t n = 0;
    while (n < max && sb->count > 0) {
        p[n++] = sb->data[sb->head];
        sb->head = (sb->head + 1) % sb->size;
        sb->count--;
    }
    return n;
}
