#include "bridge_log.h"
#include "battery.h"

#if CONFIG_BRIDGE_BATTERY

#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

static const char *TAG = "battery";

#define SAMPLES 16
#define HYSTERESIS_MV 100

static adc_oneshot_unit_handle_t s_unit;
static adc_channel_t s_chan;
static adc_cali_handle_t s_cali;
static bool s_low;

bool battery_init(void)
{
    esp_log_level_set(TAG, CONFIG_BRIDGE_LOG_LEVEL);

    adc_unit_t unit;
    if (adc_oneshot_io_to_channel(CONFIG_BRIDGE_BATTERY_GPIO, &unit, &s_chan) != ESP_OK || unit != ADC_UNIT_1) {
        ESP_LOGE(TAG, "GPIO%d non è un canale di ADC1", CONFIG_BRIDGE_BATTERY_GPIO);
        return false;
    }
    const adc_oneshot_unit_init_cfg_t unit_cfg = { .unit_id = ADC_UNIT_1 };
    const adc_oneshot_chan_cfg_t chan_cfg = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    if (adc_oneshot_new_unit(&unit_cfg, &s_unit) != ESP_OK ||
        adc_oneshot_config_channel(s_unit, s_chan, &chan_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "ADC non inizializzato");
        return false;
    }

    /* Senza calibrazione la lettura in mV non è affidabile: in quel caso la batteria risulta non misurata. */
    const adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = ADC_UNIT_1,
        .chan = s_chan,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali) != ESP_OK) {
        ESP_LOGE(TAG, "calibrazione dell'ADC non disponibile");
        return false;
    }
    ESP_LOGI(TAG, "batteria su GPIO%d, partitore x%d.%03d", CONFIG_BRIDGE_BATTERY_GPIO,
             CONFIG_BRIDGE_BATTERY_DIVIDER_X1000 / 1000, CONFIG_BRIDGE_BATTERY_DIVIDER_X1000 % 1000);
    return true;
}

uint16_t battery_read_mv(void)
{
    int sum = 0;
    int n = 0;
    for (int i = 0; i < SAMPLES; i++) {
        int mv;
        if (adc_oneshot_get_calibrated_result(s_unit, s_cali, s_chan, &mv) == ESP_OK) {
            sum += mv;
            n++;
        }
    }
    if (n == 0) {
        return 0;
    }
    uint32_t vbat = (uint32_t)(sum / n) * CONFIG_BRIDGE_BATTERY_DIVIDER_X1000 / 1000;
    ESP_LOGD(TAG, "batteria %lu mV", (unsigned long)vbat);
    return (vbat > UINT16_MAX) ? UINT16_MAX : (uint16_t)vbat;
}

bool battery_is_low(uint16_t mv)
{
    if (mv == 0) {
        s_low = false;
    } else if (mv < CONFIG_BRIDGE_BATTERY_LOW_MV) {
        s_low = true;
    } else if (mv >= CONFIG_BRIDGE_BATTERY_LOW_MV + HYSTERESIS_MV) {
        s_low = false;
    }
    return s_low;
}

#else

bool battery_init(void) { return false; }
uint16_t battery_read_mv(void) { return 0; }
bool battery_is_low(uint16_t mv) { return false; }

#endif
