/*
 * eye_battery.h implementation for the LCD 1.28: plain ADC sampling.
 *
 * This board has a charger but NO fuel gauge. The battery rail feeds GPIO1
 * (ADC1_CH0) through an on-board 1/3 voltage divider -- exactly what
 * Waveshare's own demo reads (DEC_ADC_Read * 3). Percentage is estimated
 * from a resting LiPo discharge curve, so treat it as a rough gauge: it
 * reads high while charging and sags under load.
 */

#include "eye_battery.h"

#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

static const char *TAG = "eye_bat";

#define BAT_ADC_UNIT      ADC_UNIT_1
#define BAT_ADC_CHANNEL   ADC_CHANNEL_0     /* GPIO1 on the ESP32-S3 */
#define BAT_ADC_ATTEN     ADC_ATTEN_DB_12
#define BAT_DIVIDER       3                 /* on-board divider ratio */
#define BAT_SAMPLES       8

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t         s_cali;
static bool                      s_ok;

/* Resting LiPo open-circuit voltage -> capacity. Piecewise-linear. */
static const struct { int mv; int pct; } k_curve[] = {
    { 4200, 100 }, { 4100, 94 }, { 4000, 84 }, { 3950, 78 }, { 3900, 71 },
    { 3850, 62 },  { 3800, 51 }, { 3750, 38 }, { 3700, 27 }, { 3650, 17 },
    { 3600, 9 },   { 3500, 4 },  { 3300, 0 },
};

static int mv_to_pct(int mv)
{
    const int n = sizeof(k_curve) / sizeof(k_curve[0]);
    if (mv >= k_curve[0].mv) {
        return 100;
    }
    for (int i = 1; i < n; i++) {
        if (mv >= k_curve[i].mv) {
            int dv = k_curve[i - 1].mv - k_curve[i].mv;
            int dp = k_curve[i - 1].pct - k_curve[i].pct;
            return k_curve[i].pct + (mv - k_curve[i].mv) * dp / dv;
        }
    }
    return 0;
}

esp_err_t eye_battery_init(i2c_master_bus_handle_t bus)
{
    (void)bus;   /* ADC-based board: no PMIC bus needed */
    s_ok = false;

    adc_oneshot_unit_init_cfg_t ucfg = { .unit_id = BAT_ADC_UNIT };
    esp_err_t err = adc_oneshot_new_unit(&ucfg, &s_adc);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ADC unit init failed: %s", esp_err_to_name(err));
        return err;
    }
    adc_oneshot_chan_cfg_t ccfg = {
        .atten    = BAT_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    err = adc_oneshot_config_channel(s_adc, BAT_ADC_CHANNEL, &ccfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ADC channel config failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Curve-fitting calibration is the scheme the S3 supports. Optional: if it
     * fails we fall back to raw counts scaled by the nominal reference. */
    adc_cali_curve_fitting_config_t kcfg = {
        .unit_id  = BAT_ADC_UNIT,
        .chan     = BAT_ADC_CHANNEL,
        .atten    = BAT_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&kcfg, &s_cali) != ESP_OK) {
        s_cali = NULL;
        ESP_LOGW(TAG, "no ADC calibration; using nominal scale");
    }

    s_ok = true;
    ESP_LOGI(TAG, "battery ADC ready (GPIO1, /%d divider)", BAT_DIVIDER);
    return ESP_OK;
}

bool eye_battery_read(eye_battery_t *out)
{
    if (out == NULL) {
        return false;
    }
    *out = (eye_battery_t){ 0 };
    if (!s_ok) {
        return true;
    }

    int acc = 0, got = 0;
    for (int i = 0; i < BAT_SAMPLES; i++) {
        int raw = 0;
        if (adc_oneshot_read(s_adc, BAT_ADC_CHANNEL, &raw) != ESP_OK) {
            continue;
        }
        int mv = 0;
        if (s_cali && adc_cali_raw_to_voltage(s_cali, raw, &mv) == ESP_OK) {
            acc += mv;
        } else {
            acc += raw * 3100 / 4095;   /* nominal 12 dB full scale */
        }
        got++;
    }
    if (got == 0) {
        return false;
    }

    out->mv  = (acc / got) * BAT_DIVIDER;
    out->pct = mv_to_pct(out->mv);
    /* Below ~3.0 V nothing sensible is connected (or the divider is floating);
     * report "no battery" so the info screen doesn't show a bogus 0%. */
    out->present = out->mv > 3000;
    return true;
}
