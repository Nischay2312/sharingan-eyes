/*
 * eye_battery.h implementation for the AMOLED 2.06: the board's AXP2101 PMIC.
 *
 * The AXP2101 sits on the same I2C bus as the touch controller (BSP bus,
 * address 0x34) and has a real fuel gauge: battery voltage (14-bit, 1 mV/LSB),
 * a percentage register, and charge/VBUS status bits. Register layout follows
 * the AXP2101 datasheet (same map the XPowersLib driver uses).
 */

#include "eye_battery.h"

#include "esp_log.h"

static const char *TAG = "eye_bat";

#define AXP2101_ADDR          0x34

#define REG_STATUS1           0x00  /* bit5 VBUS good, bit3 battery present   */
#define REG_STATUS2           0x01  /* bits[6:5] 01=charging, 10=discharging  */
#define REG_CHIP_ID           0x03  /* reads 0x4A on the AXP2101              */
#define REG_ADC_CH_EN         0x30  /* bit0 enables the VBAT ADC channel      */
#define REG_VBAT_H            0x34  /* VBAT[13:8]; 0x35 = VBAT[7:0]; 1 mV/LSB */
#define REG_BAT_PERCENT       0xA4  /* fuel-gauge percentage, 0..100          */

#define AXP2101_CHIP_ID       0x4A

static i2c_master_dev_handle_t s_dev;
static bool                    s_ok;

static esp_err_t reg_read(uint8_t reg, uint8_t *dst, size_t n)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, dst, n, 100);
}

static esp_err_t reg_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_dev, buf, sizeof(buf), 100);
}

esp_err_t eye_battery_init(i2c_master_bus_handle_t bus)
{
    s_ok = false;
    if (bus == NULL) {
        ESP_LOGW(TAG, "no I2C bus; battery gauge disabled");
        return ESP_ERR_INVALID_ARG;
    }

    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = AXP2101_ADDR,
        .scl_speed_hz    = 400000,
    };
    if (i2c_master_bus_add_device(bus, &cfg, &s_dev) != ESP_OK) {
        ESP_LOGW(TAG, "AXP2101: add device failed");
        return ESP_FAIL;
    }

    uint8_t id = 0;
    if (reg_read(REG_CHIP_ID, &id, 1) != ESP_OK || id != AXP2101_CHIP_ID) {
        ESP_LOGW(TAG, "AXP2101 not found (chip id 0x%02X)", id);
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
        return ESP_ERR_NOT_FOUND;
    }

    /* Make sure the VBAT ADC channel is running (read-modify-write). */
    uint8_t en = 0;
    if (reg_read(REG_ADC_CH_EN, &en, 1) == ESP_OK && !(en & 0x01)) {
        reg_write(REG_ADC_CH_EN, en | 0x01);
    }

    s_ok = true;
    ESP_LOGI(TAG, "AXP2101 fuel gauge ready");
    return ESP_OK;
}

bool eye_battery_read(eye_battery_t *out)
{
    if (out == NULL) {
        return false;
    }
    *out = (eye_battery_t){ 0 };
    if (!s_ok) {
        return true;   /* filled: no gauge -> present=false */
    }

    uint8_t st1 = 0, st2 = 0, vh = 0, vl = 0, pct = 0;
    if (reg_read(REG_STATUS1, &st1, 1) != ESP_OK ||
        reg_read(REG_STATUS2, &st2, 1) != ESP_OK ||
        reg_read(REG_VBAT_H, &vh, 1) != ESP_OK ||
        reg_read(REG_VBAT_H + 1, &vl, 1) != ESP_OK ||
        reg_read(REG_BAT_PERCENT, &pct, 1) != ESP_OK) {
        return false;
    }

    out->present  = (st1 & (1 << 3)) != 0;
    out->vbus     = (st1 & (1 << 5)) != 0;
    out->charging = ((st2 >> 5) & 0x03) == 0x01;
    out->mv       = (((int)(vh & 0x3F)) << 8) | vl;   /* H6L8, 1 mV/LSB */
    out->pct      = pct > 100 ? 100 : pct;
    return true;
}
