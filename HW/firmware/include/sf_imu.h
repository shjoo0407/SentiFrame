/* SPDX-License-Identifier: MIT */
#ifndef SF_IMU_H
#define SF_IMU_H
#include "sf_core.h"
#include "bmi270.h"
struct sf_imu { struct bmi2_dev dev; struct sf_bus bus; bool gyro; };
int sf_imu_init(struct sf_imu *imu, const struct sf_bus *bus, bool gyro);
/* Put an already-powered BMI270 into suspend, including after an MCU-only reset.
 * Does not upload the Bosch configuration image or start acquisition. */
int sf_imu_shutdown(const struct sf_bus *bus);
int sf_imu_read(struct sf_imu *imu, struct sf_sample *out, size_t cap,
                size_t *count, uint32_t now);
#endif
