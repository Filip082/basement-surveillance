#include "battery.h"

#include <stdbool.h>

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "sdkconfig.h"

#include "node_log.h"

#define T "battery"

static uint8_t percent_from_mv(int battery_mv)
{
    const int lo = CONFIG_NODE_BATTERY_MIN_MV, hi = CONFIG_NODE_BATTERY_MAX_MV;
    if (battery_mv <= lo) {
        return 0;
    }
    if (battery_mv >= hi) {
        return 100;
    }
    return (uint8_t)(((int64_t)(battery_mv - lo) * 100 + (hi - lo) / 2) / (hi - lo));
}

esp_err_t battery_measure(battery_reading_t *out)
{
    adc_unit_t unit;
    adc_channel_t chan;
    esp_err_t err = adc_oneshot_io_to_channel(CONFIG_NODE_BATTERY_ADC_GPIO, &unit, &chan);
    if (err != ESP_OK) {
        NLOGE(T, "GPIO%d has no ADC channel", CONFIG_NODE_BATTERY_ADC_GPIO);
        return err;
    }

    adc_oneshot_unit_handle_t adc = NULL;
    adc_oneshot_unit_init_cfg_t ucfg = { .unit_id = unit, .ulp_mode = ADC_ULP_MODE_DISABLE };
    err = adc_oneshot_new_unit(&ucfg, &adc);
    if (err != ESP_OK) {
        NLOGE(T, "ADC unit init failed: %s", esp_err_to_name(err));
        return err;
    }

    adc_oneshot_chan_cfg_t ccfg = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    err = adc_oneshot_config_channel(adc, chan, &ccfg);

    adc_cali_handle_t cali = NULL;
    bool have_cali = false;
    if (err == ESP_OK) {
        adc_cali_curve_fitting_config_t kcfg = {
            .unit_id = unit, .chan = chan, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        have_cali = adc_cali_create_scheme_curve_fitting(&kcfg, &cali) == ESP_OK;
        if (!have_cali) {
            NLOGW(T, "no ADC calibration available - using a rough conversion");
        }
    }

    long sum_mv = 0;
    int n = 0;
    if (err == ESP_OK) {
        int raw;
        adc_oneshot_read(adc, chan, &raw);                 /* first conversions are unreliable */
        adc_oneshot_read(adc, chan, &raw);
        for (int i = 0; i < CONFIG_NODE_BATTERY_SAMPLES; i++) {
            if (adc_oneshot_read(adc, chan, &raw) != ESP_OK) {
                continue;
            }
            int mv = 0;
            if (have_cali) {
                adc_cali_raw_to_voltage(cali, raw, &mv);
            } else {
                mv = raw * 3100 / 4095;                    /* ~12 dB range, uncalibrated */
            }
            sum_mv += mv;
            n++;
        }
    }

    if (have_cali) {
        adc_cali_delete_scheme_curve_fitting(cali);
    }
    adc_oneshot_del_unit(adc);                              /* ADC off again */

    if (err != ESP_OK || n == 0) {
        NLOGE(T, "ADC measurement failed");
        return err != ESP_OK ? err : ESP_FAIL;
    }

    int adc_mv = (int)(sum_mv / n);
    int battery_mv = (int)((int64_t)adc_mv * CONFIG_NODE_BATTERY_DIVIDER_X100 / 100);
    out->adc_mv = (uint16_t)adc_mv;
    out->battery_mv = (uint16_t)(battery_mv > 65535 ? 65535 : battery_mv);
    out->percent = percent_from_mv(battery_mv);
    NLOGI(T, "GPIO%d: %d mV at the pin (%d samples) -> %d mV battery -> %u %%",
          CONFIG_NODE_BATTERY_ADC_GPIO, adc_mv, n, battery_mv, out->percent);
    return ESP_OK;
}
