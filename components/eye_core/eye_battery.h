#pragma once

/*
 * Battery/fuel-gauge contract -- the board-specific counterpart of
 * eye_display.h. eye_core calls these; each board firmware provides an
 * implementation (there is NO default in eye_core):
 *
 *   - amoled_2_06: eye_battery_axp2101.c -- the board's AXP2101 PMIC has a
 *     real fuel gauge (voltage, percent, charging state) on the BSP I2C bus.
 *   - lcd_1_28:    eye_battery_adc.c -- no PMIC; battery voltage is sampled
 *     on GPIO1 (ADC1_CH0) through the board's /3 divider and the percentage
 *     is estimated from a LiPo discharge curve.
 *
 * Both are tolerant: with no battery attached, `present` is false and the
 * info screen shows "no battery" instead of garbage.
 */

#include <stdbool.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool present;    /* a battery is attached (or at least plausible)     */
    bool charging;   /* charge current flowing (AXP2101 only; else false) */
    bool vbus;       /* USB/VBUS power detected (AXP2101 only)            */
    int  mv;         /* battery voltage, millivolts                       */
    int  pct;        /* estimated capacity, 0..100                        */
} eye_battery_t;

/**
 * @brief Bring up the board's battery monitor.
 *
 * @param bus  I2C bus for PMIC-based boards (AMOLED: the BSP bus the AXP2101
 *             sits on). ADC-based boards ignore it -- pass NULL.
 * @return ESP_OK, or an error if the gauge is unavailable (reads then report
 *         present=false; the demo continues fine).
 */
esp_err_t eye_battery_init(i2c_master_bus_handle_t bus);

/**
 * @brief Read the current battery state. Cheap enough to call ~1 Hz.
 * @return true if `out` was filled (even if present=false), false on error.
 */
bool eye_battery_read(eye_battery_t *out);

#ifdef __cplusplus
}
#endif
