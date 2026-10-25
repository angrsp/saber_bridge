#pragma once
/* Stub minimo per provare pipe.c sul PC, a un solo thread. */
#include <assert.h>
#include <stdint.h>

typedef uint32_t TickType_t;
#define configASSERT(x) assert(x)
#define portMAX_DELAY ((TickType_t)0xffffffff)
