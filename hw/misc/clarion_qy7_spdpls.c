/*
 * Clarion QY7 (QY7221NL) speed pulse counter at 0xFFC5B000.
 *
 * Firmware proves that navdrv reads +0x0C, +0x10, +0x104 and +0x110;
 * +0x10 is the accumulated INTcount consumed by its speed worker.  The
 * pulses-per-kilometre calibration is not present in the inspected firmware,
 * so the machine supplies it as an explicitly estimated parameter.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "system/reset.h"
#include "hw/misc/clarion_qy7_spdpls.h"
#include "hw/sh4/sh.h"
#include "trace.h"

#define SPDPLS_SIZE                 0x1000
#define SPDPLS_COUNT_CLEAR          0x008
#define SPDPLS_100_LATCH            0x00c
#define SPDPLS_INT_COUNT            0x010
#define SPDPLS_STATUS_CLEAR         0x018
#define SPDPLS_500K_FREQ            0x01c
#define SPDPLS_OFFSET_B             0x104
#define SPDPLS_OBSERVE_B            0x110
#define SPDPLS_LATCH_WINDOW_NS      G_GUINT64_CONSTANT(100000000)
#define SPDPLS_NS_PER_HOUR          G_GUINT64_CONSTANT(3600000000000)

struct ClarionQy7Spdpls {
    MemoryRegion iomem;
    MemoryRegion *a7_alias;
    QEMUTimer *pulse_timer;
    QEMUTimer *latch_timer;
    qemu_irq irq;
    uint32_t speed_kmh;
    uint32_t pulses_per_km;
    uint32_t int_count;
    uint32_t latch_count;
    uint32_t latch_data;
    uint32_t filter;
    uint32_t status;
    uint32_t freq_500k;
    uint32_t offset_b;
    uint32_t observe_b;
    uint64_t period_remainder;
    uint64_t phase_remainder;
};

static uint64_t spdpls_rate(const ClarionQy7Spdpls *s)
{
    return (uint64_t)s->speed_kmh * s->pulses_per_km;
}

static uint64_t spdpls_next_period(ClarionQy7Spdpls *s)
{
    uint64_t rate = spdpls_rate(s);
    uint64_t period = SPDPLS_NS_PER_HOUR / rate;

    s->phase_remainder += s->period_remainder;
    if (s->phase_remainder >= rate) {
        s->phase_remainder -= rate;
        period++;
    }
    return MAX(period, 1);
}

static void spdpls_pulse(void *opaque)
{
    ClarionQy7Spdpls *s = opaque;
    uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    s->int_count++;
    s->latch_count++;
    trace_clarion_qy7_spdpls_pulse(s->int_count, now);
    qemu_irq_pulse(s->irq);
    timer_mod(s->pulse_timer, now + spdpls_next_period(s));
}

static void spdpls_latch(void *opaque)
{
    ClarionQy7Spdpls *s = opaque;
    uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    s->latch_data = s->latch_count;
    s->latch_count = 0;
    timer_mod(s->latch_timer, now + SPDPLS_LATCH_WINDOW_NS);
}

static void spdpls_reset(void *opaque)
{
    ClarionQy7Spdpls *s = opaque;
    uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    s->int_count = 0;
    s->latch_count = 0;
    s->latch_data = 0;
    s->filter = 0;
    s->status = 0;
    s->freq_500k = 0;
    s->offset_b = 0;
    s->observe_b = 0;
    s->phase_remainder = 0;
    timer_del(s->pulse_timer);
    timer_del(s->latch_timer);
    if (spdpls_rate(s)) {
        s->period_remainder = SPDPLS_NS_PER_HOUR % spdpls_rate(s);
        timer_mod(s->pulse_timer, now + spdpls_next_period(s));
        timer_mod(s->latch_timer, now + SPDPLS_LATCH_WINDOW_NS);
    }
}

static uint64_t spdpls_read(void *opaque, hwaddr addr, unsigned size)
{
    ClarionQy7Spdpls *s = opaque;

    switch (addr) {
    case 0x004:
        return s->filter;
    case SPDPLS_100_LATCH:
        return s->latch_data;
    case SPDPLS_INT_COUNT:
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
                      "qy7.spdpls: unimplemented device read  "
                      "(size %u, offset 0x%03" HWADDR_PRIx ")\n",
                      size, addr);
        return 0;
    }
}

static void spdpls_write(void *opaque, hwaddr addr, uint64_t value,
                         unsigned size)
{
    ClarionQy7Spdpls *s = opaque;

    switch (addr) {
    case 0x004:
        s->filter = value;
        break;
    case SPDPLS_COUNT_CLEAR:
        if (value & 1) {
            s->int_count = 0;
            s->latch_count = 0;
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
                      "qy7.spdpls: unimplemented device write "
                      "(size %u, offset 0x%03" HWADDR_PRIx
                      ", value 0x%08" PRIx64 ")\n",
                      size, addr, value);
        break;
    }
}

static const MemoryRegionOps spdpls_ops = {
    .read = spdpls_read,
    .write = spdpls_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
        .unaligned = false,
    },
};

ClarionQy7Spdpls *clarion_qy7_spdpls_init(MemoryRegion *sysmem,
                                         hwaddr base, qemu_irq irq,
                                         uint32_t speed_kmh,
                                         uint32_t pulses_per_km)
{
    ClarionQy7Spdpls *s = g_new0(ClarionQy7Spdpls, 1);

    s->irq = irq;
    s->speed_kmh = speed_kmh;
    s->pulses_per_km = pulses_per_km;
    s->period_remainder = spdpls_rate(s) ?
        SPDPLS_NS_PER_HOUR % spdpls_rate(s) : 0;
    s->pulse_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, spdpls_pulse, s);
    s->latch_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, spdpls_latch, s);
    memory_region_init_io(&s->iomem, NULL, &spdpls_ops, s, "qy7.spdpls",
                          SPDPLS_SIZE);
    memory_region_add_subregion(sysmem, base, &s->iomem);
    s->a7_alias = g_new0(MemoryRegion, 1);
    memory_region_init_alias(s->a7_alias, NULL, "qy7.spdpls-a7", &s->iomem,
                             0, SPDPLS_SIZE);
    memory_region_add_subregion(sysmem, A7ADDR(base), s->a7_alias);
    qemu_register_reset(spdpls_reset, s);
    spdpls_reset(s);
    return s;
}
