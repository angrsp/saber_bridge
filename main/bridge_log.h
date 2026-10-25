#pragma once
/* Da includere al posto di esp_log.h: compila i log del bridge al livello scelto nel menu
 * "Saber Bridge", indipendentemente dal livello massimo di ESP-IDF. Ogni modulo chiama poi
 * esp_log_level_set(TAG, CONFIG_BRIDGE_LOG_LEVEL) all'avvio. */
#include "sdkconfig.h"
#define LOG_LOCAL_LEVEL CONFIG_BRIDGE_LOG_LEVEL
#include "esp_log.h"
