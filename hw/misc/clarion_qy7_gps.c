/*
 * Time-only binary GPS receiver model for Clarion QY7 SCIF7.
 * The A0 time fields and framing follow the QY7 navigation driver.
 * Unknown navigation fields remain zero. The receiver ignores host commands.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include <math.h>
#include "qemu/log.h"
#include "qemu/timer.h"
#include "system/rtc.h"
#include "hw/misc/clarion_qy7_gps.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"

#define QY7_GPS_FRAME_SIZE 36
#define QY7_GPS_PERIOD_NS NANOSECONDS_PER_SECOND
#define QY7_GPS_BYTE_NS (10 * NANOSECONDS_PER_SECOND / 4800)
#define QY7_GPS_TX_RETRY_NS (10 * 1000 * 1000)
#define QY7_GPS_RX_MAX 16

struct ClarionQy7GpsState {
    DeviceState parent_obj;
    ClarionQy7GpsSink sink;
    void *sink_opaque;
    QEMUTimer *tick;
    QEMUTimer *tx_timer;
    uint8_t tx[QY7_GPS_FRAME_SIZE];
    unsigned tx_offset;
    bool tx_pending;
    unsigned tx_accepted;
    unsigned tx_retries;
    bool rollover;
    uint8_t status;
    double lat;
    double lon;
    uint8_t rx[QY7_GPS_RX_MAX];
    unsigned rx_len;
};

static void qy7_gps_put_coordinate(uint8_t *out, double degrees)
{
    int32_t coordinate = llround(degrees * 60000.0);
    uint32_t raw = (uint32_t)coordinate & 0x0fffffff;

    for (int i = 0; i < 4; i++) {
        out[i] = (raw >> (21 - 7 * i)) & 0x7f;
    }
}

static void qy7_gps_trace_frame(const char *direction, const uint8_t *buf,
                                unsigned len)
{
    char hex[QY7_GPS_FRAME_SIZE * 3 + 1];
    unsigned i;

    for (i = 0; i < len; i++) {
        g_snprintf(hex + i * 3, sizeof(hex) - i * 3, "%02x%s", buf[i],
                   i + 1 == len ? "" : " ");
    }
    qemu_log_mask(LOG_UNIMP, "clarion-qy7-gps: %s %s\n", direction, hex);
}

static void qy7_gps_tx_pump(void *opaque)
{
    ClarionQy7GpsState *s = opaque;
    int accepted;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (!s->sink || !s->tx_pending) {
        return;
    }
    accepted = s->sink(s->sink_opaque, s->tx + s->tx_offset, 1);
    if (!accepted) {
        s->tx_retries++;
        timer_mod(s->tx_timer, now + QY7_GPS_TX_RETRY_NS);
        return;
    }
    s->tx_accepted += accepted;
    s->tx_offset += accepted;
    if (s->tx_offset < QY7_GPS_FRAME_SIZE) {
        timer_mod(s->tx_timer,
                  now + QY7_GPS_BYTE_NS);
        return;
    }
    qemu_log_mask(LOG_UNIMP,
                  "clarion-qy7-gps: frame complete accepted=%u retries=%u\n",
                  s->tx_accepted, s->tx_retries);
    s->tx_pending = false;
    s->tx_offset = 0;
    s->tx_accepted = 0;
    s->tx_retries = 0;
}

static void qy7_gps_make_frame(ClarionQy7GpsState *s)
{
    struct tm now;
    GDateTime *date;
    int year, month, day, i;
    unsigned sum = 0;

    memset(s->tx, 0, sizeof(s->tx));
    s->tx[0] = 0xa0;
    s->tx[1] = s->status;
    qy7_gps_put_coordinate(&s->tx[2], s->lat);
    qy7_gps_put_coordinate(&s->tx[6], s->lon);
    qemu_get_timedate(&now, 0);
    year = now.tm_year + 1900;
    month = now.tm_mon + 1;
    day = now.tm_mday;
    if (s->rollover) {
        date = g_date_time_new_utc(year, month, day, now.tm_hour, now.tm_min,
                                   now.tm_sec);
        if (date) {
            GDateTime *adjusted = g_date_time_add_days(date, -7168);

            g_date_time_unref(date);
            date = adjusted;
        }
        if (date) {
            year = g_date_time_get_year(date);
            month = g_date_time_get_month(date);
            day = g_date_time_get_day_of_month(date);
            g_date_time_unref(date);
        }
    }
    s->tx[0x10] = year % 100;
    s->tx[0x11] = month;
    s->tx[0x12] = day;
    s->tx[0x13] = now.tm_hour;
    s->tx[0x14] = now.tm_min;
    s->tx[0x15] = now.tm_sec;
    s->tx[0x23] = 0xf0;
    for (i = 1; i <= 0x21; i++) {
        sum += s->tx[i];
    }
    s->tx[0x22] = sum & 0x7f;
    s->tx_pending = true;
    s->tx_offset = 0;
    s->tx_accepted = 0;
    s->tx_retries = 0;
    qy7_gps_trace_frame("TX", s->tx, sizeof(s->tx));
    if (s->sink) {
        timer_mod(s->tx_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + QY7_GPS_BYTE_NS);
    }
}

static void qy7_gps_tick(void *opaque)
{
    ClarionQy7GpsState *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (!s->tx_pending) {
        qy7_gps_make_frame(s);
    }
    timer_mod(s->tick, now + QY7_GPS_PERIOD_NS);
}

void clarion_qy7_gps_set_sink(DeviceState *dev, ClarionQy7GpsSink fn,
                              void *opaque)
{
    ClarionQy7GpsState *s = CLARION_QY7_GPS(dev);

    s->sink = fn;
    s->sink_opaque = opaque;
    if (fn && s->tx_pending) {
        timer_mod(s->tx_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + QY7_GPS_BYTE_NS);
    }
}

void clarion_qy7_gps_set_position(DeviceState *dev, double lat, double lon)
{
    ClarionQy7GpsState *s = CLARION_QY7_GPS(dev);

    s->lat = lat;
    s->lon = lon;
}

void clarion_qy7_gps_rx_byte(DeviceState *dev, uint8_t byte)
{
    ClarionQy7GpsState *s = CLARION_QY7_GPS(dev);

    if (!s->rx_len && byte != 0xb0) {
        return;
    }
    if (s->rx_len == sizeof(s->rx)) {
        s->rx_len = 0;
    }
    s->rx[s->rx_len++] = byte;
    if (byte == 0xf0) {
        qy7_gps_trace_frame("RX ignored (no reply modeled)", s->rx, s->rx_len);
        s->rx_len = 0;
    }
}

static void qy7_gps_realize(DeviceState *dev, Error **errp)
{
    ClarionQy7GpsState *s = CLARION_QY7_GPS(dev);

    s->tick = timer_new_ns(QEMU_CLOCK_VIRTUAL, qy7_gps_tick, s);
    s->tx_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, qy7_gps_tx_pump, s);
    timer_mod(s->tick,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + QY7_GPS_PERIOD_NS);
}

static void qy7_gps_unrealize(DeviceState *dev)
{
    ClarionQy7GpsState *s = CLARION_QY7_GPS(dev);

    timer_free(s->tick);
    timer_free(s->tx_timer);
}

static const VMStateDescription vmstate_clarion_qy7_gps = {
    .name = TYPE_CLARION_QY7_GPS,
    .unmigratable = 1,
};

static const Property qy7_gps_properties[] = {
    DEFINE_PROP_BOOL("rollover", ClarionQy7GpsState, rollover, true),
    DEFINE_PROP_UINT8("status", ClarionQy7GpsState, status, 0x01),
};

static void qy7_gps_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = qy7_gps_realize;
    dc->unrealize = qy7_gps_unrealize;
    dc->vmsd = &vmstate_clarion_qy7_gps;
    device_class_set_props(dc, qy7_gps_properties);
}

static const TypeInfo qy7_gps_typeinfo = {
    .name = TYPE_CLARION_QY7_GPS,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(ClarionQy7GpsState),
    .class_init = qy7_gps_class_init,
};

static void qy7_gps_register_types(void)
{
    type_register_static(&qy7_gps_typeinfo);
}

type_init(qy7_gps_register_types)
