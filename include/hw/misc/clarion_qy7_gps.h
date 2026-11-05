/*
 * Minimal time-only JRC GPS receiver model for Clarion QY7.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_CLARION_QY7_GPS_H
#define HW_MISC_CLARION_QY7_GPS_H

#include "qemu/osdep.h"
#include "hw/core/qdev.h"

typedef int (*ClarionQy7GpsSink)(void *opaque, const uint8_t *buf, int len);

#define TYPE_CLARION_QY7_GPS "clarion-qy7-gps"
OBJECT_DECLARE_SIMPLE_TYPE(ClarionQy7GpsState, CLARION_QY7_GPS)

void clarion_qy7_gps_set_sink(DeviceState *dev, ClarionQy7GpsSink fn,
                              void *opaque);
void clarion_qy7_gps_set_position(DeviceState *dev, double lat, double lon);
void clarion_qy7_gps_rx_byte(DeviceState *dev, uint8_t byte);

#endif
