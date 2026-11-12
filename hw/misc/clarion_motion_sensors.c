/*
 * Clarion QY7/QY8 speed pulse and gyro sensor model.
 *
 * SPDPLS pulse calibration is 5096 pulses/km. QY7 receives a pulse through
 * INT2; QY8 routes a level IRQ directly to GIC SPI 109. The gyro exposes
 * five ADC registers at offsets 0x30..0x40.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include <math.h>
#include "qemu/log.h"
#include "qemu/timer.h"
#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "hw/misc/clarion_motion_sensors.h"
#include "hw/sh4/sh.h"
#include "trace.h"

#define MOTION_A_SIZE 0x1000
#define MOTION_B_SIZE 0x1000
#define MOTION_TICK_NS G_GUINT64_CONSTANT(100000000)
#define MOTION_NS_PER_HOUR G_GUINT64_CONSTANT(3600000000000)
#define MOTION_GYRO_ZERO 0x5000
#define MOTION_GYRO_SCALE 0.00483
#define MOTION_GYRO_MAX_DELTA 12000.0

#define SPDPLS_COUNT_CLEAR 0x008
#define SPDPLS_100_LATCH 0x00c
#define SPDPLS_INT_COUNT 0x010
#define SPDPLS_STATUS_CLEAR 0x018
#define SPDPLS_500K_FREQ 0x01c
#define SPDPLS_OFFSET_B 0x104
#define SPDPLS_OBSERVE_B 0x110

struct ClarionMotionSensorsState {
    SysBusDevice parent_obj;
    MemoryRegion gyro_iomem;
    MemoryRegion spdpls_iomem;
    MemoryRegion *gyro_alias;
    MemoryRegion *spdpls_alias;
    QEMUTimer *pulse_timer;
    QEMUTimer *latch_timer;
    QEMUTimer *motion_timer;
    qemu_irq irq;
    uint32_t speed_kmh;
    uint32_t pulses_per_km;
    uint32_t int_count;
    uint32_t pulse_total;
    uint32_t latch_data;
    uint32_t filter;
    uint32_t status;
    uint32_t freq_500k;
    uint32_t offset_b;
    uint32_t observe_b;
    uint32_t gyro_raw;
    uint64_t period_remainder;
    uint64_t phase_remainder;
    int64_t motion_last_ns;
    double odometer_m;
    double yaw_rate;
    bool level_irq;
    bool a7_alias;
};

static uint64_t motion_rate(const ClarionMotionSensorsState *s)
{
    return (uint64_t)s->speed_kmh * s->pulses_per_km;
}

static uint64_t motion_next_period(ClarionMotionSensorsState *s)
{
    uint64_t rate = motion_rate(s);
    uint64_t period = MOTION_NS_PER_HOUR / rate;

    s->phase_remainder += s->period_remainder;
    if (s->phase_remainder >= rate) {
        s->phase_remainder -= rate;
        period++;
    }
    return MAX(period, 1);
}

static void motion_pulse(void *opaque)
{
    ClarionMotionSensorsState *s = opaque;
    uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    s->pulse_total++;
    trace_clarion_motion_sensors_pulse(s->pulse_total, now);
    timer_mod(s->pulse_timer, now + motion_next_period(s));
}

static void motion_latch(void *opaque)
{
    ClarionMotionSensorsState *s = opaque;
    uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    s->latch_data = s->pulse_total & 0xffff;
    s->int_count++;
    if (s->level_irq) {
        qemu_irq_raise(s->irq);
    } else {
        qemu_irq_pulse(s->irq);
    }
    timer_mod(s->latch_timer, now + MOTION_TICK_NS);
}

static void motion_tick(void *opaque)
{
    ClarionMotionSensorsState *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int64_t elapsed_ns = now - s->motion_last_ns;
    double elapsed_s = elapsed_ns > 0 ? elapsed_ns / 1e9 : 0.0;

    s->motion_last_ns = now;
    if (s->speed_kmh && elapsed_s > 0.0) {
        s->odometer_m += s->speed_kmh * (elapsed_s / 3.6);
    }
    if (s->yaw_rate != 0.0 && s->speed_kmh > 0) {
        double delta = s->yaw_rate / MOTION_GYRO_SCALE;

        delta = CLAMP(delta, -MOTION_GYRO_MAX_DELTA, MOTION_GYRO_MAX_DELTA);
        s->gyro_raw = MOTION_GYRO_ZERO + lrint(delta);
    } else {
        s->gyro_raw = MOTION_GYRO_ZERO;
    }
    timer_mod(s->motion_timer, now + MOTION_TICK_NS);
}

static void motion_reset(void *opaque)
{
    ClarionMotionSensorsState *s = opaque;
    uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint64_t rate = motion_rate(s);

    s->int_count = 0;
    s->pulse_total = 0;
    s->latch_data = 0;
    s->filter = 0;
    s->status = 0;
    s->freq_500k = 0;
    s->offset_b = 0;
    s->observe_b = 0;
    s->gyro_raw = MOTION_GYRO_ZERO;
    s->phase_remainder = 0;
    s->odometer_m = 0.0;
    s->motion_last_ns = now;
    if (s->level_irq) {
        qemu_irq_lower(s->irq);
    }
    timer_del(s->pulse_timer);
    timer_del(s->latch_timer);
    timer_del(s->motion_timer);
    s->period_remainder = rate ? MOTION_NS_PER_HOUR % rate : 0;
    if (rate) {
        timer_mod(s->pulse_timer, now + motion_next_period(s));
    }
    timer_mod(s->latch_timer, now + MOTION_TICK_NS);
    timer_mod(s->motion_timer, now + MOTION_TICK_NS);
}

static void motion_device_reset(DeviceState *dev)
{
    motion_reset(dev);
}

static void motion_set_speed(ClarionMotionSensorsState *s, uint32_t speed_kmh)
{
    uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint64_t rate;

    s->speed_kmh = speed_kmh;
    s->phase_remainder = 0;
    timer_del(s->pulse_timer);
    rate = motion_rate(s);
    s->period_remainder = rate ? MOTION_NS_PER_HOUR % rate : 0;
    if (rate) {
        timer_mod(s->pulse_timer, now + motion_next_period(s));
    }
}

void clarion_motion_sensors_set_speed(DeviceState *dev, uint32_t speed_kmh)
{
    motion_set_speed(CLARION_MOTION_SENSORS(dev), speed_kmh);
}

void clarion_motion_sensors_set_yaw_rate(DeviceState *dev, double yaw_rate)
{
    CLARION_MOTION_SENSORS(dev)->yaw_rate = yaw_rate;
}

void clarion_motion_sensors_set_gyro_raw(DeviceState *dev, uint32_t gyro_raw)
{
    CLARION_MOTION_SENSORS(dev)->gyro_raw = gyro_raw;
}

uint32_t clarion_motion_sensors_get_gyro_raw(DeviceState *dev)
{
    return CLARION_MOTION_SENSORS(dev)->gyro_raw;
}

double clarion_motion_sensors_get_odometer_m(DeviceState *dev)
{
    return CLARION_MOTION_SENSORS(dev)->odometer_m;
}

static uint64_t motion_gyro_read(void *opaque, hwaddr addr, unsigned size)
{
    ClarionMotionSensorsState *s = opaque;

    switch (addr) {
    case 0x030:
        return s->gyro_raw;
    case 0x034:
    case 0x038:
        return 0x5000;
    case 0x03c:
    case 0x040:
        return 0x4000;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "clarion-motion-sensors: unimplemented gyro read "
                      "(size %u, offset 0x%03" HWADDR_PRIx ")\n",
                      size, addr);
        return 0;
    }
}

static void motion_gyro_write(void *opaque, hwaddr addr, uint64_t value,
                              unsigned size)
{
    qemu_log_mask(LOG_UNIMP,
                  "clarion-motion-sensors: accepted gyro initialization "
                  "write (size %u, offset 0x%03" HWADDR_PRIx
                  ", value 0x%08" PRIx64 ")\n",
                  size, addr, value);
}

static uint64_t motion_spdpls_read(void *opaque, hwaddr addr, unsigned size)
{
    ClarionMotionSensorsState *s = opaque;

    switch (addr) {
    case 0x004:
        return s->filter;
    case SPDPLS_100_LATCH:
        return s->latch_data;
    case SPDPLS_INT_COUNT:
        if (s->level_irq) {
            /* Hypothesis: the guest acknowledges the level by reading +0x10. */
            qemu_irq_lower(s->irq);
        }
        return s->int_count;
    case SPDPLS_STATUS_CLEAR:
        return s->status;
    case SPDPLS_500K_FREQ:
        return s->freq_500k;
    case SPDPLS_OFFSET_B:
        return s->offset_b;
    case SPDPLS_OBSERVE_B:
        return s->observe_b;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "clarion-motion-sensors: unimplemented SPDPLS read "
                      "(size %u, offset 0x%03" HWADDR_PRIx ")\n",
                      size, addr);
        return 0;
    }
}

static void motion_spdpls_write(void *opaque, hwaddr addr, uint64_t value,
                                unsigned size)
{
    ClarionMotionSensorsState *s = opaque;

    switch (addr) {
    case 0x004:
        s->filter = value;
        break;
    case SPDPLS_COUNT_CLEAR:
        if (value & 1) {
            s->int_count = 0;
            s->pulse_total = 0;
            s->latch_data = 0;
        }
        break;
    case SPDPLS_STATUS_CLEAR:
        if (value & 1) {
            s->status = 0;
        }
        break;
    case SPDPLS_500K_FREQ:
        s->freq_500k = value;
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "clarion-motion-sensors: unimplemented SPDPLS write "
                      "(size %u, offset 0x%03" HWADDR_PRIx
                      ", value 0x%08" PRIx64 ")\n",
                      size, addr, value);
        break;
    }
}

static const MemoryRegionOps motion_gyro_ops = {
    .read = motion_gyro_read,
    .write = motion_gyro_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
        .unaligned = false,
    },
};

static const MemoryRegionOps motion_spdpls_ops = {
    .read = motion_spdpls_read,
    .write = motion_spdpls_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
        .unaligned = false,
    },
};

static void clarion_motion_sensors_realize(DeviceState *dev, Error **errp)
{
    ClarionMotionSensorsState *s = CLARION_MOTION_SENSORS(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    (void)errp;
    memory_region_init_io(&s->gyro_iomem, OBJECT(s), &motion_gyro_ops, s,
                          "clarion-motion-sensors.gyro", MOTION_A_SIZE);
    memory_region_init_io(&s->spdpls_iomem, OBJECT(s), &motion_spdpls_ops, s,
                          "clarion-motion-sensors.spdpls", MOTION_B_SIZE);
    sysbus_init_mmio(sbd, &s->gyro_iomem);
    sysbus_init_mmio(sbd, &s->spdpls_iomem);
    sysbus_init_irq(sbd, &s->irq);
    if (s->a7_alias) {
        s->gyro_alias = g_new0(MemoryRegion, 1);
        s->spdpls_alias = g_new0(MemoryRegion, 1);
        memory_region_init_alias(s->gyro_alias, OBJECT(s),
                                 "clarion-motion-sensors.gyro-a7",
                                 &s->gyro_iomem, 0, MOTION_A_SIZE);
        memory_region_init_alias(s->spdpls_alias, OBJECT(s),
                                 "clarion-motion-sensors.spdpls-a7",
                                 &s->spdpls_iomem, 0, MOTION_B_SIZE);
        sysbus_init_mmio(sbd, s->gyro_alias);
        sysbus_init_mmio(sbd, s->spdpls_alias);
    }
    s->pulse_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, motion_pulse, s);
    s->latch_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, motion_latch, s);
    s->motion_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, motion_tick, s);
    motion_reset(s);
}

static const Property clarion_motion_sensors_properties[] = {
    DEFINE_PROP_BOOL("level-irq", ClarionMotionSensorsState, level_irq, false),
    DEFINE_PROP_BOOL("a7-alias", ClarionMotionSensorsState, a7_alias, false),
    DEFINE_PROP_UINT32("speed-kmh", ClarionMotionSensorsState, speed_kmh, 0),
    DEFINE_PROP_UINT32("pulses-per-km", ClarionMotionSensorsState,
                       pulses_per_km, 5096),
    DEFINE_PROP_UINT32("gyro-raw", ClarionMotionSensorsState, gyro_raw,
                       MOTION_GYRO_ZERO),
};

static void clarion_motion_sensors_class_init(ObjectClass *klass,
                                              const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    (void)data;
    dc->realize = clarion_motion_sensors_realize;
    device_class_set_legacy_reset(dc, motion_device_reset);
    device_class_set_props(dc, clarion_motion_sensors_properties);
}

static const TypeInfo clarion_motion_sensors_info = {
    .name = TYPE_CLARION_MOTION_SENSORS,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ClarionMotionSensorsState),
    .class_init = clarion_motion_sensors_class_init,
};

static void clarion_motion_sensors_register_types(void)
{
    type_register_static(&clarion_motion_sensors_info);
}

type_init(clarion_motion_sensors_register_types)
