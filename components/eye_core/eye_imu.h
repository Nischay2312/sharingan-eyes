#pragma once

/*
 * Board-agnostic QMI8658 (6-axis IMU) helper + simple tilt trigger.
 *
 * The QMI8658 is the SAME chip on both target boards (AMOLED 2.06 and the round
 * 1.28"), so this driver is shared. Only the I2C *bus handle* differs per board,
 * and that is passed in by the board layer -- this file pulls in no BSP.
 *
 * The tilt trigger here is intentionally minimal: it reports whether the board
 * is currently tilted past a threshold, which the demo uses to switch between
 * the two animations. If the IMU is absent or fails to probe, the demo still
 * runs and eye_imu_tilted() simply always returns false.
 */

#include <stdbool.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Probe and configure the QMI8658 on the given I2C master bus.
 *
 * Tolerant: if the chip is not found this returns an error but leaves the
 * module in a safe state where eye_imu_tilted() returns false.
 *
 * @param bus  I2C master bus handle from the board BSP (e.g. bsp_i2c_get_handle()).
 */
esp_err_t eye_imu_init(i2c_master_bus_handle_t bus);

/** @return true if the IMU was found and configured. */
bool eye_imu_present(void);

/**
 * @brief Read the accelerometer in g.
 * @return true on a successful read (values written), false otherwise.
 */
bool eye_imu_read_accel_g(float *ax, float *ay, float *az);

/**
 * @brief Read the gyroscope in degrees/second.
 * @return true on a successful read (values written), false otherwise.
 */
bool eye_imu_read_gyro_dps(float *gx, float *gy, float *gz);

/**
 * @brief Simple tilt test: true when the board is tilted away from face-up by
 *        more than ~30 degrees. Returns false if no IMU is present.
 *        (Kept for reference; the demo uses the flick gesture instead.)
 */
bool eye_imu_tilted(void);

/**
 * @brief Flick-gesture detector. Call repeatedly (e.g. at 100 Hz). Reports each
 *        quick wrist flick ONCE: a brief, high angular-rate spike. Latches until
 *        motion settles below the re-arm threshold, with a short cooldown, so
 *        one flick == one event.
 *
 * @param hi_dps  Trigger threshold (deg/s) -- a deliberate flick exceeds this.
 * @param lo_dps  Re-arm threshold (deg/s)  -- motion must drop below this to
 *                allow the next event.
 * @return +1 or -1 (the flick's rotation direction) once per detected flick,
 *         0 otherwise. Flicking the opposite way returns the opposite sign,
 *         which the demo maps to cycling forward/backward.
 */
int eye_imu_poll_flick(int hi_dps, int lo_dps);

#ifdef __cplusplus
}
#endif
