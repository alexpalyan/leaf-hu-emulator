/*
 * Clarion QY7/QY8 motion sensor device.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_MISC_CLARION_MOTION_SENSORS_H
#define HW_MISC_CLARION_MOTION_SENSORS_H

#include "hw/core/qdev.h"

#define TYPE_CLARION_MOTION_SENSORS "clarion-motion-sensors"
OBJECT_DECLARE_SIMPLE_TYPE(ClarionMotionSensorsState, CLARION_MOTION_SENSORS)

void clarion_motion_sensors_set_speed(DeviceState *dev, uint32_t speed_kmh);
void clarion_motion_sensors_set_yaw_rate(DeviceState *dev, double yaw_rate);
void clarion_motion_sensors_set_gyro_raw(DeviceState *dev, uint32_t gyro_raw);
uint32_t clarion_motion_sensors_get_gyro_raw(DeviceState *dev);

#endif
