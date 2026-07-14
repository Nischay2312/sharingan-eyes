#include "eye_imu.h"

#include <math.h>
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "eye_imu";

/* QMI8658 register map (subset). */
#define QMI8658_REG_WHO_AM_I   0x00
#define QMI8658_REG_CTRL1      0x02
#define QMI8658_REG_CTRL2      0x03   /* accel: full-scale + ODR */
#define QMI8658_REG_CTRL3      0x04   /* gyro:  full-scale + ODR */
#define QMI8658_REG_CTRL7      0x08   /* sensor enable */
#define QMI8658_REG_AX_L       0x35   /* accel X..Z, 6 bytes, little-endian */
#define QMI8658_REG_GX_L       0x3B   /* gyro  X..Z, 6 bytes, little-endian */

#define QMI8658_WHO_AM_I_VAL   0x05

/* CTRL2: accel full scale +/-4g (001b in bits[6:4]) + ODR ~250 Hz (0x03). */
#define QMI8658_CTRL2_ACC_4G   0x13
/* +/-4g => 8192 LSB/g. */
#define QMI8658_ACC_LSB_PER_G  8192.0f

/* CTRL3: gyro full scale +/-2048 dps (111b in bits[6:4]) + ODR ~224 Hz (0x05).
 * Wide range so a fast flick does not clip. +/-2048 dps => 16 LSB/dps. */
#define QMI8658_CTRL3_GYR_2048 0x75
#define QMI8658_GYR_LSB_PER_DPS 16.0f

/* The 7-bit address depends on the SA0 pin; probe both common values. */
static const uint8_t k_addr_candidates[] = { 0x6B, 0x6A };

static i2c_master_dev_handle_t s_dev;
static bool                    s_present;

static esp_err_t reg_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_dev, buf, sizeof(buf), 100);
}

static esp_err_t reg_read(uint8_t reg, uint8_t *dst, size_t n)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, dst, n, 100);
}

esp_err_t eye_imu_init(i2c_master_bus_handle_t bus)
{
    s_present = false;
    if (bus == NULL) {
        ESP_LOGW(TAG, "no I2C bus supplied; tilt disabled");
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t i = 0; i < sizeof(k_addr_candidates); i++) {
        i2c_device_config_t cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address  = k_addr_candidates[i],
            .scl_speed_hz    = 400000,
        };
        if (s_dev != NULL) {
            i2c_master_bus_rm_device(s_dev);
            s_dev = NULL;
        }
        if (i2c_master_bus_add_device(bus, &cfg, &s_dev) != ESP_OK) {
            continue;
        }
        uint8_t who = 0;
        if (reg_read(QMI8658_REG_WHO_AM_I, &who, 1) == ESP_OK && who == QMI8658_WHO_AM_I_VAL) {
            ESP_LOGI(TAG, "QMI8658 found at 0x%02X", k_addr_candidates[i]);
            s_present = true;
            break;
        }
    }

    if (!s_present) {
        ESP_LOGW(TAG, "QMI8658 not detected; tilt switching disabled");
        if (s_dev != NULL) {
            i2c_master_bus_rm_device(s_dev);
            s_dev = NULL;
        }
        return ESP_ERR_NOT_FOUND;
    }

    /* CTRL1: enable register address auto-increment, little-endian data. */
    reg_write(QMI8658_REG_CTRL1, 0x40);
    /* CTRL2: accelerometer range/ODR. */
    reg_write(QMI8658_REG_CTRL2, QMI8658_CTRL2_ACC_4G);
    /* CTRL3: gyroscope range/ODR (used by the flick gesture). */
    reg_write(QMI8658_REG_CTRL3, QMI8658_CTRL3_GYR_2048);
    /* CTRL7: enable accelerometer (bit0) + gyroscope (bit1). */
    reg_write(QMI8658_REG_CTRL7, 0x03);
    return ESP_OK;
}

bool eye_imu_present(void)
{
    return s_present;
}

bool eye_imu_read_accel_g(float *ax, float *ay, float *az)
{
    if (!s_present) {
        return false;
    }
    uint8_t raw[6];
    if (reg_read(QMI8658_REG_AX_L, raw, sizeof(raw)) != ESP_OK) {
        return false;
    }
    int16_t x = (int16_t)((uint16_t)raw[0] | ((uint16_t)raw[1] << 8));
    int16_t y = (int16_t)((uint16_t)raw[2] | ((uint16_t)raw[3] << 8));
    int16_t z = (int16_t)((uint16_t)raw[4] | ((uint16_t)raw[5] << 8));
    if (ax) {
        *ax = (float)x / QMI8658_ACC_LSB_PER_G;
    }
    if (ay) {
        *ay = (float)y / QMI8658_ACC_LSB_PER_G;
    }
    if (az) {
        *az = (float)z / QMI8658_ACC_LSB_PER_G;
    }
    return true;
}

bool eye_imu_read_gyro_dps(float *gx, float *gy, float *gz)
{
    if (!s_present) {
        return false;
    }
    uint8_t raw[6];
    if (reg_read(QMI8658_REG_GX_L, raw, sizeof(raw)) != ESP_OK) {
        return false;
    }
    int16_t x = (int16_t)((uint16_t)raw[0] | ((uint16_t)raw[1] << 8));
    int16_t y = (int16_t)((uint16_t)raw[2] | ((uint16_t)raw[3] << 8));
    int16_t z = (int16_t)((uint16_t)raw[4] | ((uint16_t)raw[5] << 8));
    if (gx) {
        *gx = (float)x / QMI8658_GYR_LSB_PER_DPS;
    }
    if (gy) {
        *gy = (float)y / QMI8658_GYR_LSB_PER_DPS;
    }
    if (gz) {
        *gz = (float)z / QMI8658_GYR_LSB_PER_DPS;
    }
    return true;
}

bool eye_imu_tilted(void)
{
    float ax, ay, az;
    if (!eye_imu_read_accel_g(&ax, &ay, &az)) {
        return false;
    }
    /*
     * Face-up rests gravity on one axis (|az| ~ 1g). Tilt past ~30 degrees moves
     * enough gravity into the X/Y plane that the horizontal component exceeds
     * sin(30) = 0.5g. Using the horizontal magnitude makes the test independent
     * of which way the board is mounted in the mask.
     */
    float horiz = sqrtf(ax * ax + ay * ay);
    return horiz > 0.5f;
}

/* Minimum time between two accepted flicks (prevents the return swing of a
 * single back-and-forth gesture from registering as a second event). */
#define FLICK_COOLDOWN_US 500000

int eye_imu_poll_flick(int hi_dps, int lo_dps)
{
    static bool    s_armed = true;
    static int64_t s_last_us = 0;

    float gx, gy, gz;
    if (!eye_imu_read_gyro_dps(&gx, &gy, &gz)) {
        return 0;
    }

    /* Peak axis rate: a wrist flick spikes one axis hard. Keep the SIGNED
     * value of that axis -- the flick's rotation direction picks forward or
     * backward cycling. (The same physical twist always spikes the same axis
     * with the same sign first; the return swing lands in the cooldown.) */
    float peak = gx;
    if (fabsf(gy) > fabsf(peak)) {
        peak = gy;
    }
    if (fabsf(gz) > fabsf(peak)) {
        peak = gz;
    }
    float m = fabsf(peak);

    int64_t now = esp_timer_get_time();
    if (m < (float)lo_dps) {
        s_armed = true;                 /* motion settled -> ready again */
    }
    if (s_armed && m > (float)hi_dps && (now - s_last_us) > FLICK_COOLDOWN_US) {
        s_armed   = false;
        s_last_us = now;
        return peak >= 0.0f ? 1 : -1;
    }
    return 0;
}
