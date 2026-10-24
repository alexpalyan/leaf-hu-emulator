/*
 * Sierra Wireless USB CDC ACM modem used by the Clarion QY8 TCU model.
 *
 * Copyright (c) 2026
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qemu/bswap.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/usb/usb.h"
#include "net/net.h"
#include "migration/vmstate.h"
#include "chardev/char-fe.h"
#include "qom/object.h"
#include "qapi/visitor.h"
#include "desc.h"
#include "trace.h"

#define TYPE_USB_CDC_ACM "usb-cdc-acm"
OBJECT_DECLARE_SIMPLE_TYPE(USBCDCACMState, USB_CDC_ACM)

#define CDC_ACM_SET_LINE_CODING 0x20
#define CDC_ACM_GET_LINE_CODING 0x21
#define CDC_ACM_SET_CONTROL_LINE 0x22
#define CDC_ACM_SEND_BREAK 0x23
#define CDC_ACM_SERIAL_STATE 0x20
#define CDC_ACM_RX_CARRIER (1u << 0)
#define CDC_ACM_TX_CARRIER (1u << 1)
#define CDC_ACM_RX_SIZE 8192
#define CDC_ACM_EP_INTERRUPT 4
#define CDC_ACM_EP_BULK_IN 5
#define CDC_ACM_EP_BULK_OUT 4
#define CDC_ACM_EP_SECONDARY_BULK_IN 7
#define CDC_ACM_EP_SECONDARY_BULK_OUT 5
#define CDC_ACM_EP_INTERRUPT_SECONDARY 6
#define CDC_ACM_EP_INTERRUPT_SIZE 64
#define CDC_ACM_NOTIFICATION_SIZE 10
#define CDC_ACM_PPP_FRAME_SIZE 2048
#define CDC_ACM_PPP_IP 0x0021
#define CDC_ACM_PPP_LCP 0xc021
#define CDC_ACM_PPP_IPCP 0x8021
#define CDC_ACM_PPP_FCS_INIT 0xffff
#define CDC_ACM_PPP_FCS_GOOD 0xf0b8
#define CDC_ACM_PPP_FCS_XOR 0xffff
#define CDC_ACM_PPP_FLAG 0x7e
#define CDC_ACM_PPP_ESCAPE 0x7d
#define CDC_ACM_PPP_ESCAPE_XOR 0x20
#define CDC_ACM_PPP_GUEST_IP 0x0a00020f
#define CDC_ACM_PPP_GATEWAY_IP 0x0a000202
#define CDC_ACM_PPP_DNS_IP 0x0a000203

struct USBCDCACMState {
    USBDevice dev;
    USBEndpoint *intr;
    USBEndpoint *intr_secondary;
    USBEndpoint *bulk_in;
    USBEndpoint *bulk_in_secondary;
    CharFrontend chr;
    NICConf conf;
    NICState *nic;
    uint8_t rx_buf[CDC_ACM_RX_SIZE];
    uint32_t rx_head;
    uint32_t rx_count;
    uint32_t baud;
    uint8_t stop_bits;
    uint8_t parity;
    uint8_t data_bits;
    uint8_t dtr;
    uint8_t rts;
    uint16_t serial_state;
    bool serial_state_pending;
    bool data_mode;
    bool lcp_peer_acked;
    bool lcp_our_acked;
    bool ipcp_peer_acked;
    bool ipcp_our_acked;
    bool peer_pfc;
    bool peer_acfc;
    uint8_t at_buf[128];
    size_t at_len;
    uint8_t ppp_frame[CDC_ACM_PPP_FRAME_SIZE];
    size_t ppp_frame_len;
    bool ppp_escape;
    uint8_t pending_ip[CDC_ACM_PPP_FRAME_SIZE];
    size_t pending_ip_len;
    uint8_t gateway_mac[6];
    bool gateway_mac_valid;
};

static NetClientInfo net_cdc_acm_info;

enum {
    STR_CONFIGURATION = 1,
    STR_PRODUCT,
    STR_MANUFACTURER,
};

static const USBDescStrings cdc_acm_strings = {
    [STR_CONFIGURATION] = "Sierra Configuration",
    [STR_PRODUCT] = "AR8550",
    [STR_MANUFACTURER] = "Sierra Wireless, Incorporated",
};

static const USBDescIface cdc_acm_ifaces[] = {
    {
        .bInterfaceNumber = 0,
        .bNumEndpoints = 2,
        .bInterfaceClass = USB_CLASS_VENDOR_SPEC,
        .bInterfaceSubClass = USB_CLASS_VENDOR_SPEC,
        .bInterfaceProtocol = USB_CLASS_VENDOR_SPEC,
        .eps =
            (USBDescEndpoint[]){
                {
                    .bEndpointAddress = USB_DIR_IN | 1,
                    .bmAttributes = USB_ENDPOINT_XFER_BULK,
                    .wMaxPacketSize = 512,
                },
                {
                    .bEndpointAddress = USB_DIR_OUT | 1,
                    .bmAttributes = USB_ENDPOINT_XFER_BULK,
                    .wMaxPacketSize = 512,
                },
            },
    },
    {
        .bInterfaceNumber = 1,
        .bNumEndpoints = 2,
        .bInterfaceClass = USB_CLASS_VENDOR_SPEC,
        .bInterfaceSubClass = USB_CLASS_VENDOR_SPEC,
        .bInterfaceProtocol = USB_CLASS_VENDOR_SPEC,
        .eps =
            (USBDescEndpoint[]){
                {
                    .bEndpointAddress = USB_DIR_IN | 2,
                    .bmAttributes = USB_ENDPOINT_XFER_BULK,
                    .wMaxPacketSize = 512,
                },
                {
                    .bEndpointAddress = USB_DIR_OUT | 2,
                    .bmAttributes = USB_ENDPOINT_XFER_BULK,
                    .wMaxPacketSize = 512,
                },
            },
    },
    {
        .bInterfaceNumber = 2,
        .bNumEndpoints = 2,
        .bInterfaceClass = USB_CLASS_VENDOR_SPEC,
        .bInterfaceSubClass = USB_CLASS_VENDOR_SPEC,
        .bInterfaceProtocol = USB_CLASS_VENDOR_SPEC,
        .eps =
            (USBDescEndpoint[]){
                {
                    .bEndpointAddress = USB_DIR_IN | 3,
                    .bmAttributes = USB_ENDPOINT_XFER_BULK,
                    .wMaxPacketSize = 512,
                },
                {
                    .bEndpointAddress = USB_DIR_OUT | 3,
                    .bmAttributes = USB_ENDPOINT_XFER_BULK,
                    .wMaxPacketSize = 512,
                },
            },
    },
    {
        .bInterfaceNumber = 3,
        .bNumEndpoints = 3,
        .bInterfaceClass = USB_CLASS_COMM,
        .bInterfaceSubClass = 2,
        .bInterfaceProtocol = 1,
        .eps =
            (USBDescEndpoint[]){
                {
                    .bEndpointAddress = USB_DIR_IN | CDC_ACM_EP_INTERRUPT,
                    .bmAttributes = USB_ENDPOINT_XFER_INT,
                    .wMaxPacketSize = CDC_ACM_EP_INTERRUPT_SIZE,
                    .bInterval = 5,
                },
                {
                    .bEndpointAddress = USB_DIR_IN | CDC_ACM_EP_BULK_IN,
                    .bmAttributes = USB_ENDPOINT_XFER_BULK,
                    .wMaxPacketSize = 512,
                },
                {
                    .bEndpointAddress = USB_DIR_OUT | CDC_ACM_EP_BULK_OUT,
                    .bmAttributes = USB_ENDPOINT_XFER_BULK,
                    .wMaxPacketSize = 512,
                },
            },
    },
    {
        .bInterfaceNumber = 4,
        .bNumEndpoints = 3,
        .bInterfaceClass = USB_CLASS_COMM,
        .bInterfaceSubClass = 2,
        .bInterfaceProtocol = 1,
        .eps =
            (USBDescEndpoint[]){
                {
                    .bEndpointAddress =
                        USB_DIR_IN | CDC_ACM_EP_INTERRUPT_SECONDARY,
                    .bmAttributes = USB_ENDPOINT_XFER_INT,
                    .wMaxPacketSize = CDC_ACM_EP_INTERRUPT_SIZE,
                    .bInterval = 5,
                },
                {
                    .bEndpointAddress =
                        USB_DIR_IN | CDC_ACM_EP_SECONDARY_BULK_IN,
                    .bmAttributes = USB_ENDPOINT_XFER_BULK,
                    .wMaxPacketSize = 512,
                },
                {
                    .bEndpointAddress =
                        USB_DIR_OUT | CDC_ACM_EP_SECONDARY_BULK_OUT,
                    .bmAttributes = USB_ENDPOINT_XFER_BULK,
                    .wMaxPacketSize = 512,
                },
            },
    },
    {
        .bInterfaceNumber = 5,
        .bNumEndpoints = 3,
        .bInterfaceClass = USB_CLASS_VENDOR_SPEC,
        .bInterfaceSubClass = USB_CLASS_VENDOR_SPEC,
        .bInterfaceProtocol = USB_CLASS_VENDOR_SPEC,
        .eps =
            (USBDescEndpoint[]){
                {
                    .bEndpointAddress = USB_DIR_IN | 8,
                    .bmAttributes = USB_ENDPOINT_XFER_INT,
                    .wMaxPacketSize = 64,
                    .bInterval = 5,
                },
                {
                    .bEndpointAddress = USB_DIR_IN | 9,
                    .bmAttributes = USB_ENDPOINT_XFER_BULK,
                    .wMaxPacketSize = 512,
                },
                {
                    .bEndpointAddress = USB_DIR_OUT | 6,
                    .bmAttributes = USB_ENDPOINT_XFER_BULK,
                    .wMaxPacketSize = 512,
                },
            },
    },
    {
        .bInterfaceNumber = 6,
        .bNumEndpoints = 3,
        .bInterfaceClass = USB_CLASS_VENDOR_SPEC,
        .bInterfaceSubClass = USB_CLASS_VENDOR_SPEC,
        .bInterfaceProtocol = USB_CLASS_VENDOR_SPEC,
        .eps =
            (USBDescEndpoint[]){
                {
                    .bEndpointAddress = USB_DIR_IN | 10,
                    .bmAttributes = USB_ENDPOINT_XFER_INT,
                    .wMaxPacketSize = 64,
                    .bInterval = 5,
                },
                {
                    .bEndpointAddress = USB_DIR_IN | 11,
                    .bmAttributes = USB_ENDPOINT_XFER_BULK,
                    .wMaxPacketSize = 512,
                },
                {
                    .bEndpointAddress = USB_DIR_OUT | 7,
                    .bmAttributes = USB_ENDPOINT_XFER_BULK,
                    .wMaxPacketSize = 512,
                },
            },
    },
};

static const USBDescDevice cdc_acm_device = {
    .bcdUSB = 0x0200,
    .bDeviceClass = 0,
    .bDeviceSubClass = 0,
    .bDeviceProtocol = 0,
    .bMaxPacketSize0 = 64,
    .bNumConfigurations = 1,
    .confs =
        (USBDescConfig[]){
            {
                .bNumInterfaces = ARRAY_SIZE(cdc_acm_ifaces),
                .bConfigurationValue = 1,
                .iConfiguration = STR_CONFIGURATION,
                .bmAttributes = USB_CFG_ATT_ONE,
                .bMaxPower = 0,
                .nif = ARRAY_SIZE(cdc_acm_ifaces),
                .ifs = cdc_acm_ifaces,
            },
        },
};

static const USBDesc cdc_acm_desc = {
    .id = {
        .idVendor = 0x1199,
        .idProduct = 0x68ab,
        .bcdDevice = 0x0006,
        .iManufacturer = STR_MANUFACTURER,
        .iProduct = STR_PRODUCT,
        .iSerialNumber = 0,
    },
    .full = &cdc_acm_device,
    .high = &cdc_acm_device,
    .str = cdc_acm_strings,
};

static char *cdc_acm_hex(const uint8_t *buf, size_t len)
{
    GString *str = g_string_sized_new(len * 2 + 1);
    size_t i;

    for (i = 0; i < len; i++) {
        g_string_append_printf(str, "%02x", buf[i]);
    }
    return g_string_free(str, false);
}

static void cdc_acm_trace_control(USBPacket *p, int request, int value,
                                  int index, int length, const uint8_t *data,
                                  size_t data_len)
{
    char *hex = cdc_acm_hex(data, data_len);
    trace_usb_cdc_acm_control((request >> 8) & 0xff, request & 0xff,
                              value & 0xffff, index & 0xffff, length & 0xffff,
                              p->status, hex);
    g_free(hex);
}

static void cdc_acm_trace_bytes(const char *event, const char *direction,
                                const uint8_t *data, size_t len)
{
    char *hex = cdc_acm_hex(data, len);
    trace_usb_cdc_acm_bytes(event, direction, len, hex);
    g_free(hex);
}

static void cdc_acm_set_serial_state(USBCDCACMState *s, uint16_t state)
{
    state &= CDC_ACM_RX_CARRIER | CDC_ACM_TX_CARRIER;
    if (state == s->serial_state) {
        return;
    }

    s->serial_state = state;
    s->serial_state_pending = true;
    trace_usb_cdc_acm_serial_state("set", !!(state & CDC_ACM_RX_CARRIER),
                                   !!(state & CDC_ACM_TX_CARRIER), state);
    usb_wakeup(s->intr, 0);
    usb_wakeup(s->intr_secondary, 0);
}

static uint16_t cdc_acm_ppp_fcs(uint16_t fcs, const uint8_t *data, size_t len)
{
    size_t i;
    int bit;

    for (i = 0; i < len; i++) {
        fcs ^= data[i];
        for (bit = 0; bit < 8; bit++) {
            fcs = (fcs & 1) ? (fcs >> 1) ^ 0x8408 : fcs >> 1;
        }
    }
    return fcs;
}

static void cdc_acm_queue_rx(USBCDCACMState *s, const uint8_t *data, size_t len)
{
    uint32_t tail = (s->rx_head + s->rx_count) % CDC_ACM_RX_SIZE;
    size_t first;

    len = MIN(len, CDC_ACM_RX_SIZE - s->rx_count);
    first = MIN(len, CDC_ACM_RX_SIZE - tail);
    memcpy(s->rx_buf + tail, data, first);
    if (len > first) {
        memcpy(s->rx_buf, data + first, len - first);
    }
    s->rx_count += len;
    usb_wakeup(s->bulk_in, 0);
    usb_wakeup(s->bulk_in_secondary, 0);
}

static void cdc_acm_ppp_send(USBCDCACMState *s, uint16_t protocol,
                             const uint8_t *payload, size_t len)
{
    uint8_t frame[CDC_ACM_PPP_FRAME_SIZE];
    uint8_t encoded[CDC_ACM_PPP_FRAME_SIZE * 2 + 2];
    size_t frame_len = len + 6;
    size_t encoded_len = 0;
    size_t i;
    uint16_t fcs;

    if (frame_len > sizeof(frame)) {
        return;
    }
    frame[0] = 0xff;
    frame[1] = 0x03;
    frame[2] = protocol >> 8;
    frame[3] = protocol;
    memcpy(frame + 4, payload, len);
    fcs = cdc_acm_ppp_fcs(CDC_ACM_PPP_FCS_INIT, frame, len + 4);
    fcs ^= CDC_ACM_PPP_FCS_XOR;
    frame[len + 4] = fcs;
    frame[len + 5] = fcs >> 8;
    encoded[encoded_len++] = CDC_ACM_PPP_FLAG;
    for (i = 0; i < frame_len; i++) {
        if (frame[i] == CDC_ACM_PPP_FLAG || frame[i] == CDC_ACM_PPP_ESCAPE ||
            frame[i] < 0x20) {
            encoded[encoded_len++] = CDC_ACM_PPP_ESCAPE;
            encoded[encoded_len++] = frame[i] ^ CDC_ACM_PPP_ESCAPE_XOR;
        } else {
            encoded[encoded_len++] = frame[i];
        }
    }
    encoded[encoded_len++] = CDC_ACM_PPP_FLAG;
    cdc_acm_queue_rx(s, encoded, encoded_len);
}

static void cdc_acm_ppp_control(USBCDCACMState *s, uint16_t protocol,
                                uint8_t code, uint8_t id, const uint8_t *data,
                                size_t len)
{
    uint8_t packet[CDC_ACM_PPP_FRAME_SIZE];

    if (len + 4 > sizeof(packet)) {
        return;
    }
    packet[0] = code;
    packet[1] = id;
    stw_be_p(packet + 2, len + 4);
    memcpy(packet + 4, data, len);
    cdc_acm_ppp_send(s, protocol, packet, len + 4);
}

static void cdc_acm_ppp_start_ipcp(USBCDCACMState *s)
{
    uint8_t options[] = {
        3, 6, 10, 0, 2, 2, 129, 6, 10, 0, 2, 3, 131, 6, 10, 0, 2, 3,
    };

    if (s->lcp_peer_acked && s->lcp_our_acked && !s->ipcp_our_acked) {
        cdc_acm_ppp_control(s, CDC_ACM_PPP_IPCP, 1, 2, options,
                            sizeof(options));
    }
}

static void cdc_acm_ppp_lcp_request(USBCDCACMState *s)
{
    uint8_t options[] = {
        1, 4, 0x05, 0xdc, 5, 6, 0x51, 0x59, 0x38, 0x32,
    };

    if (!s->lcp_our_acked) {
        cdc_acm_ppp_control(s, CDC_ACM_PPP_LCP, 1, 1, options, sizeof(options));
    }
}

static void cdc_acm_nic_send_arp_request(USBCDCACMState *s)
{
    uint8_t packet[42] = {
        0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0, 0, 0, 0, 0,  0, 0x08, 0x06,
        0,    1,    0x08, 0,    6,    4,    0, 1, 0, 0, 0,  0, 0,    0,
        10,   0,    2,    15,   0,    0,    0, 0, 0, 0, 10, 0, 2,    2,
    };

    if (!s->nic) {
        return;
    }
    memcpy(packet + 6, s->conf.macaddr.a, 6);
    qemu_send_packet(qemu_get_queue(s->nic), packet, sizeof(packet));
}

static void cdc_acm_nic_send_ip(USBCDCACMState *s, const uint8_t *ip,
                                size_t len)
{
    uint8_t packet[CDC_ACM_PPP_FRAME_SIZE + 14];

    if (!s->nic || len > CDC_ACM_PPP_FRAME_SIZE) {
        return;
    }
    if (!s->gateway_mac_valid) {
        memcpy(s->pending_ip, ip, len);
        s->pending_ip_len = len;
        cdc_acm_nic_send_arp_request(s);
        return;
    }
    memcpy(packet, s->gateway_mac, 6);
    memcpy(packet + 6, s->conf.macaddr.a, 6);
    packet[12] = 0x08;
    packet[13] = 0x00;
    memcpy(packet + 14, ip, len);
    qemu_send_packet(qemu_get_queue(s->nic), packet, len + 14);
}

static void cdc_acm_ppp_ip(USBCDCACMState *s, const uint8_t *ip, size_t len)
{
    if (s->ipcp_peer_acked && s->ipcp_our_acked && len >= 20 &&
        ldl_be_p(ip + 12) == CDC_ACM_PPP_GUEST_IP) {
        cdc_acm_nic_send_ip(s, ip, len);
    }
}

static void cdc_acm_ppp_ipcp(USBCDCACMState *s, const uint8_t *packet,
                             size_t len)
{
    uint8_t response[CDC_ACM_PPP_FRAME_SIZE];
    uint8_t rejected[CDC_ACM_PPP_FRAME_SIZE];
    size_t response_len = 0;
    size_t rejected_len = 0;
    size_t offset = 4;
    bool nak = false;
    uint16_t declared;

    if (len < 4) {
        return;
    }
    declared = lduw_be_p(packet + 2);
    if (declared < 4 || declared > len) {
        return;
    }
    if (packet[0] == 1) {
        while (offset + 2 <= declared) {
            uint8_t option_len = packet[offset + 1];
            uint32_t wanted;

            if (option_len < 2 || offset + option_len > declared) {
                return;
            }
            if (packet[offset] == 3 && option_len == 6) {
                wanted = CDC_ACM_PPP_GUEST_IP;
            } else if ((packet[offset] == 129 || packet[offset] == 131) &&
                       option_len == 6) {
                wanted = CDC_ACM_PPP_DNS_IP;
            } else if (packet[offset] == 3 || packet[offset] == 129 ||
                       packet[offset] == 131) {
                return;
            } else {
                memcpy(rejected + rejected_len, packet + offset, option_len);
                rejected_len += option_len;
                offset += option_len;
                continue;
            }
            if (ldl_be_p(packet + offset + 2) != wanted) {
                uint8_t *value = response + response_len;

                memcpy(value, packet + offset, option_len);
                stl_be_p(value + 2, wanted);
                response_len += option_len;
                nak = true;
            } else {
                memcpy(response + response_len, packet + offset, option_len);
                response_len += option_len;
            }
            offset += option_len;
        }
        if (offset != declared) {
            return;
        }
        if (rejected_len) {
            cdc_acm_ppp_control(s, CDC_ACM_PPP_IPCP, 4, packet[1], rejected,
                                rejected_len);
            s->ipcp_peer_acked = false;
        } else {
            cdc_acm_ppp_control(s, CDC_ACM_PPP_IPCP, nak ? 3 : 2, packet[1],
                                response, response_len);
            s->ipcp_peer_acked = !nak;
        }
        cdc_acm_ppp_start_ipcp(s);
    } else if (packet[0] == 2 && packet[1] == 2) {
        s->ipcp_our_acked = true;
    } else if (packet[0] == 4) {
        cdc_acm_ppp_control(s, CDC_ACM_PPP_IPCP, 5, packet[1], packet + 4,
                            declared - 4);
    }
}

static void cdc_acm_ppp_lcp(USBCDCACMState *s, const uint8_t *packet,
                            size_t len)
{
    uint8_t rejected[CDC_ACM_PPP_FRAME_SIZE];
    size_t rejected_len = 0;
    size_t offset;
    bool valid = true;
    uint16_t declared;

    if (len < 4) {
        return;
    }
    declared = lduw_be_p(packet + 2);
    if (declared < 4 || declared > len) {
        return;
    }
    if (packet[0] == 1) {
        s->peer_pfc = false;
        s->peer_acfc = false;
        for (offset = 4; offset + 2 <= declared;) {
            uint8_t option_type = packet[offset];
            uint8_t option_len = packet[offset + 1];

            if (option_len < 2 || offset + option_len > declared) {
                valid = false;
                break;
            }
            if ((option_type == 1 && option_len != 4) ||
                (option_type == 2 && option_len != 6) ||
                (option_type == 5 && option_len != 6) ||
                ((option_type == 7 || option_type == 8) && option_len != 2)) {
                valid = false;
                break;
            }
            if (option_type == 7) {
                s->peer_pfc = true;
            } else if (option_type == 8) {
                s->peer_acfc = true;
            } else if (option_type != 1 && option_type != 2 &&
                       option_type != 5) {
                memcpy(rejected + rejected_len, packet + offset, option_len);
                rejected_len += option_len;
            }
            offset += option_len;
        }
        if (!valid || offset != declared) {
            return;
        }
        if (rejected_len) {
            s->peer_pfc = false;
            s->peer_acfc = false;
            cdc_acm_ppp_control(s, CDC_ACM_PPP_LCP, 4, packet[1], rejected,
                                rejected_len);
            s->lcp_peer_acked = false;
        } else {
            cdc_acm_ppp_control(s, CDC_ACM_PPP_LCP, 2, packet[1], packet + 4,
                                declared - 4);
            s->lcp_peer_acked = true;
        }
        cdc_acm_ppp_lcp_request(s);
    } else if (packet[0] == 2 && packet[1] == 1) {
        s->lcp_our_acked = true;
    } else if (packet[0] == 9 && declared >= 8) {
        cdc_acm_ppp_control(s, CDC_ACM_PPP_LCP, 10, packet[1], packet + 4,
                            declared - 4);
    } else if (packet[0] == 5) {
        cdc_acm_ppp_control(s, CDC_ACM_PPP_LCP, 6, packet[1], packet + 4,
                            declared - 4);
    } else if (packet[0] == 6) {
        s->data_mode = false;
        cdc_acm_set_serial_state(s, 0);
        cdc_acm_queue_rx(s, (const uint8_t *)"\r\nNO CARRIER\r\n", 14);
    }
    if (s->lcp_peer_acked && s->lcp_our_acked) {
        cdc_acm_ppp_start_ipcp(s);
    }
}

static void cdc_acm_ppp_frame(USBCDCACMState *s)
{
    uint16_t protocol;
    const uint8_t *payload;
    size_t len = s->ppp_frame_len;
    size_t offset = 0;

    if (len < 6 || cdc_acm_ppp_fcs(CDC_ACM_PPP_FCS_INIT, s->ppp_frame, len) !=
                       CDC_ACM_PPP_FCS_GOOD) {
        return;
    }
    len -= 2;
    if (len >= 2 && s->ppp_frame[0] == 0xff && s->ppp_frame[1] == 0x03) {
        offset = 2;
    } else if (!s->peer_acfc) {
        return;
    }
    if (len <= offset) {
        return;
    }
    if ((s->ppp_frame[offset] & 1) && s->peer_pfc) {
        protocol = s->ppp_frame[offset++];
    } else if (offset + 2 <= len) {
        protocol = lduw_be_p(s->ppp_frame + offset);
        offset += 2;
    } else {
        return;
    }
    payload = s->ppp_frame + offset;
    len -= offset;
    if (protocol == CDC_ACM_PPP_LCP) {
        cdc_acm_ppp_lcp(s, payload, len);
    } else if (protocol == CDC_ACM_PPP_IPCP) {
        cdc_acm_ppp_ipcp(s, payload, len);
    } else if (protocol == CDC_ACM_PPP_IP) {
        cdc_acm_ppp_ip(s, payload, len);
    }
}

static void cdc_acm_ppp_byte(USBCDCACMState *s, uint8_t byte)
{
    if (byte == CDC_ACM_PPP_FLAG) {
        if (s->ppp_frame_len) {
            cdc_acm_ppp_frame(s);
        }
        s->ppp_frame_len = 0;
        s->ppp_escape = false;
        return;
    }
    if (s->ppp_escape) {
        byte ^= CDC_ACM_PPP_ESCAPE_XOR;
        s->ppp_escape = false;
    } else if (byte == CDC_ACM_PPP_ESCAPE) {
        s->ppp_escape = true;
        return;
    }
    if (s->ppp_frame_len < sizeof(s->ppp_frame)) {
        s->ppp_frame[s->ppp_frame_len++] = byte;
    } else {
        s->ppp_frame_len = 0;
    }
}

static void cdc_acm_modem_command(USBCDCACMState *s)
{
    char command[sizeof(s->at_buf) + 1];
    const char *reply = "\r\nERROR\r\n";
    size_t i;

    memcpy(command, s->at_buf, s->at_len);
    command[s->at_len] = '\0';
    for (i = 0; i < s->at_len; i++) {
        command[i] = g_ascii_toupper(command[i]);
    }
    if (!strcmp(command, "AT+CGDCONT=?")) {
        reply = "\r\n+CGDCONT: (1-3),(\"IP\",\"IPV6\",\"IPV4V6\")\r\nOK\r\n";
    } else if (!strcmp(command, "AT+CGDCONT?")) {
        reply = "\r\n+CGDCONT: 1,\"IP\",\"internet\",\"0.0.0.0\",0,0\r\nOK\r\n";
    } else if (!strcmp(command, "AT") || !strcmp(command, "ATE0&C1&D2") ||
               !strcmp(command, "ATX4") || !strcmp(command, "ATS7=60") ||
               !strcmp(command, "AT+CMER?") ||
               g_str_has_prefix(command, "AT+CMER=") ||
               !strcmp(command, "AT+CNUM") ||
               g_str_has_prefix(command, "AT+CGDCONT=")) {
        reply = "\r\nOK\r\n";
    } else if (!strcmp(command, "ATI")) {
        reply = "\r\nTCU Gen2 Kai CX.6_RC01\r\nOK\r\n";
    } else if (g_str_has_prefix(command, "ATD*99***") &&
               command[s->at_len - 1] == '#') {
        reply = "\r\nCONNECT\r\n";
        s->data_mode = true;
        s->lcp_peer_acked = false;
        s->lcp_our_acked = false;
        s->ipcp_peer_acked = false;
        s->ipcp_our_acked = false;
        s->peer_pfc = false;
        s->peer_acfc = false;
        s->pending_ip_len = 0;
        s->ppp_frame_len = 0;
        cdc_acm_set_serial_state(s, CDC_ACM_RX_CARRIER | CDC_ACM_TX_CARRIER);
    } else if (!strcmp(command, "ATH")) {
        reply = "\r\nOK\r\n";
        s->data_mode = false;
        cdc_acm_set_serial_state(s, 0);
    }
    cdc_acm_queue_rx(s, (const uint8_t *)reply, strlen(reply));
    if (s->data_mode) {
        cdc_acm_ppp_lcp_request(s);
    }
    s->at_len = 0;
}

static void cdc_acm_builtin_write(USBCDCACMState *s, const uint8_t *buf,
                                  size_t len)
{
    size_t i;

    for (i = 0; i < len; i++) {
        if (s->data_mode) {
            cdc_acm_ppp_byte(s, buf[i]);
        } else if (buf[i] == '\r' || buf[i] == '\n') {
            if (s->at_len) {
                cdc_acm_modem_command(s);
            }
        } else if (s->at_len < sizeof(s->at_buf)) {
            s->at_buf[s->at_len++] = buf[i];
        }
    }
}

static ssize_t cdc_acm_nic_receive(NetClientState *nc, const uint8_t *buf,
                                   size_t size)
{
    USBCDCACMState *s = qemu_get_nic_opaque(nc);
    uint16_t ethertype;

    if (size < 14) {
        return size;
    }
    ethertype = lduw_be_p(buf + 12);
    if (ethertype == 0x0806 && size >= 42 && lduw_be_p(buf + 14) == 1 &&
        lduw_be_p(buf + 16) == 0x0800 && buf[18] == 6 && buf[19] == 4) {
        uint32_t sender_ip = ldl_be_p(buf + 28);
        uint32_t target_ip = ldl_be_p(buf + 38);

        if (sender_ip == CDC_ACM_PPP_GATEWAY_IP) {
            memcpy(s->gateway_mac, buf + 22, 6);
            s->gateway_mac_valid = true;
            if (s->pending_ip_len) {
                size_t pending_len = s->pending_ip_len;

                s->pending_ip_len = 0;
                cdc_acm_nic_send_ip(s, s->pending_ip, pending_len);
            }
        }
        if (lduw_be_p(buf + 20) == 1 && target_ip == CDC_ACM_PPP_GUEST_IP) {
            uint8_t reply[42];

            memcpy(reply, buf, sizeof(reply));
            memcpy(reply, buf + 22, 6);
            memcpy(reply + 6, s->conf.macaddr.a, 6);
            stw_be_p(reply + 20, 2);
            memcpy(reply + 22, s->conf.macaddr.a, 6);
            stl_be_p(reply + 28, CDC_ACM_PPP_GUEST_IP);
            memcpy(reply + 32, buf + 22, 6);
            stl_be_p(reply + 38, sender_ip);
            qemu_send_packet(qemu_get_queue(s->nic), reply, sizeof(reply));
        }
    } else if (ethertype == 0x0800 && size > 34 && s->data_mode &&
               s->ipcp_peer_acked && s->ipcp_our_acked &&
               ldl_be_p(buf + 30) == CDC_ACM_PPP_GUEST_IP) {
        cdc_acm_ppp_send(s, CDC_ACM_PPP_IP, buf + 14, size - 14);
    }
    return size;
}

static void cdc_acm_get_state(Object *obj, Visitor *v, const char *name,
                              void *opaque, Error **errp)
{
    USBCDCACMState *s = USB_CDC_ACM(obj);
    uint32_t state = s->serial_state;

    (void)opaque;
    visit_type_uint32(v, name, &state, errp);
}

static void cdc_acm_set_state(Object *obj, Visitor *v, const char *name,
                              void *opaque, Error **errp)
{
    USBCDCACMState *s = USB_CDC_ACM(obj);
    uint32_t state;

    (void)opaque;
    if (!visit_type_uint32(v, name, &state, errp)) {
        return;
    }
    if (state & ~(CDC_ACM_RX_CARRIER | CDC_ACM_TX_CARRIER)) {
        error_setg(errp,
                   "serial-state accepts only DCD (bit 0) and DSR (bit 1)");
        return;
    }
    cdc_acm_set_serial_state(s, state);
}

static void cdc_acm_reset(USBDevice *dev)
{
    USBCDCACMState *s = USB_CDC_ACM(dev);

    s->rx_head = 0;
    s->rx_count = 0;
    s->baud = 9600;
    s->stop_bits = 0;
    s->parity = 0;
    s->data_bits = 8;
    s->dtr = 0;
    s->rts = 0;
    s->serial_state = 0;
    s->serial_state_pending = false;
    s->data_mode = false;
    s->lcp_peer_acked = false;
    s->lcp_our_acked = false;
    s->ipcp_peer_acked = false;
    s->ipcp_our_acked = false;
    s->peer_pfc = false;
    s->peer_acfc = false;
    s->at_len = 0;
    s->ppp_frame_len = 0;
    s->ppp_escape = false;
    s->pending_ip_len = 0;
    s->gateway_mac_valid = false;
    trace_usb_cdc_acm_event("reset", 0, 0);
}

static void cdc_acm_handle_control(USBDevice *dev, USBPacket *p, int request,
                                   int value, int index, int length,
                                   uint8_t *data)
{
    USBCDCACMState *s = USB_CDC_ACM(dev);
    size_t data_len = 0;
    int ret;

    ret = usb_desc_handle_control(dev, p, request, value, index, length, data);
    if (ret >= 0) {
        data_len = (request >> 8) & USB_DIR_IN ? p->actual_length : length;
        cdc_acm_trace_control(p, request, value, index, length, data, data_len);
        return;
    }

    switch (request) {
    case ClassInterfaceOutRequest | CDC_ACM_SET_LINE_CODING:
        if ((index != 3 && index != 4) || length != 7) {
            goto stall;
        }
        s->baud = ldl_le_p(data);
        s->stop_bits = data[4];
        s->parity = data[5];
        s->data_bits = data[6];
        data_len = length;
        cdc_acm_trace_control(p, request, value, index, length, data, data_len);
        trace_usb_cdc_acm_line_coding("set", s->baud, s->stop_bits, s->parity,
                                      s->data_bits);
        return;
    case ClassInterfaceRequest | CDC_ACM_GET_LINE_CODING:
        if ((index != 3 && index != 4) || length < 7) {
            goto stall;
        }
        stl_le_p(data, s->baud);
        data[4] = s->stop_bits;
        data[5] = s->parity;
        data[6] = s->data_bits;
        p->actual_length = 7;
        cdc_acm_trace_control(p, request, value, index, length, data, 7);
        trace_usb_cdc_acm_line_coding("get", s->baud, s->stop_bits, s->parity,
                                      s->data_bits);
        return;
    case ClassInterfaceOutRequest | CDC_ACM_SET_CONTROL_LINE:
        if (index != 3 && index != 4) {
            goto stall;
        }
        if (s->data_mode && !(value & 1)) {
            s->data_mode = false;
            cdc_acm_set_serial_state(s, 0);
            cdc_acm_queue_rx(s, (const uint8_t *)"\r\nNO CARRIER\r\n", 14);
        }
        s->dtr = value & 1;
        s->rts = (value >> 1) & 1;
        cdc_acm_trace_control(p, request, value, index, length, NULL, 0);
        trace_usb_cdc_acm_control_lines(s->dtr, s->rts);
        return;
    case ClassInterfaceOutRequest | CDC_ACM_SEND_BREAK:
        cdc_acm_trace_control(p, request, value, index, length, NULL, 0);
        trace_usb_cdc_acm_event("send_break", value & 0xffff, 0);
        if (s->data_mode) {
            s->data_mode = false;
            cdc_acm_set_serial_state(s, 0);
            cdc_acm_queue_rx(s, (const uint8_t *)"\r\nNO CARRIER\r\n", 14);
        }
        return;
    case InterfaceRequest | USB_REQ_GET_INTERFACE:
        data[0] = 0;
        p->actual_length = 1;
        cdc_acm_trace_control(p, request, value, index, length, data, 1);
        return;
    case InterfaceOutRequest | USB_REQ_SET_INTERFACE:
    case EndpointOutRequest | USB_REQ_CLEAR_FEATURE:
        cdc_acm_trace_control(p, request, value, index, length, NULL, 0);
        return;
    default:
        break;
    }

stall:
    p->status = USB_RET_STALL;
    cdc_acm_trace_control(p, request, value, index, length, data,
                          (request >> 8) & USB_DIR_IN ? p->actual_length
                                                      : length);
}

static void cdc_acm_handle_data(USBDevice *dev, USBPacket *p)
{
    USBCDCACMState *s = USB_CDC_ACM(dev);
    uint8_t ep = p->ep->nr;
    uint8_t buf[CDC_ACM_RX_SIZE];
    size_t len;

    if (p->pid == USB_TOKEN_OUT &&
        (ep == CDC_ACM_EP_BULK_OUT || ep == CDC_ACM_EP_SECONDARY_BULK_OUT)) {
        len = usb_packet_size(p);
        if (len > sizeof(buf)) {
            p->status = USB_RET_STALL;
            return;
        }
        usb_packet_copy(p, buf, len);
        p->actual_length = len;
        cdc_acm_trace_bytes("bulk", "out", buf, len);
        if (qemu_chr_fe_backend_connected(&s->chr)) {
            qemu_chr_fe_write_all(&s->chr, buf, len);
        } else if (s->nic) {
            cdc_acm_builtin_write(s, buf, len);
        }
        return;
    }

    if (p->pid == USB_TOKEN_IN &&
        (ep == CDC_ACM_EP_BULK_IN || ep == CDC_ACM_EP_SECONDARY_BULK_IN)) {
        uint8_t out[CDC_ACM_RX_SIZE];

        if (!s->rx_count) {
            p->status = USB_RET_NAK;
            return;
        }
        len = MIN((uint32_t)usb_packet_size(p), s->rx_count);
        for (size_t i = 0; i < len; i++) {
            out[i] = s->rx_buf[(s->rx_head + i) % CDC_ACM_RX_SIZE];
        }
        usb_packet_copy(p, out, len);
        s->rx_head = (s->rx_head + len) % CDC_ACM_RX_SIZE;
        s->rx_count -= len;
        p->actual_length = len;
        cdc_acm_trace_bytes("bulk", "in", out, len);
        return;
    }

    if (p->pid == USB_TOKEN_IN &&
        (ep == CDC_ACM_EP_INTERRUPT || ep == CDC_ACM_EP_INTERRUPT_SECONDARY)) {
        uint8_t notification[CDC_ACM_NOTIFICATION_SIZE] = {
            0xa1, CDC_ACM_SERIAL_STATE, 0, 0, 0, 0, 2, 0, 0, 0,
        };

        if (!s->serial_state_pending) {
            p->status = USB_RET_NAK;
            return;
        }
        stw_le_p(notification + 8, s->serial_state);
        usb_packet_copy(p, notification, sizeof(notification));
        p->actual_length = sizeof(notification);
        s->serial_state_pending = false;
        cdc_acm_trace_bytes("interrupt", "in", notification,
                            sizeof(notification));
        trace_usb_cdc_acm_serial_state(
            "sent", !!(s->serial_state & CDC_ACM_RX_CARRIER),
            !!(s->serial_state & CDC_ACM_TX_CARRIER), s->serial_state);
        return;
    }

    p->status = USB_RET_STALL;
}

static int cdc_acm_can_read(void *opaque)
{
    USBCDCACMState *s = opaque;

    return CDC_ACM_RX_SIZE - s->rx_count;
}

static void cdc_acm_read(void *opaque, const uint8_t *buf, int size)
{
    USBCDCACMState *s = opaque;
    uint32_t tail = (s->rx_head + s->rx_count) % CDC_ACM_RX_SIZE;
    uint32_t first;

    size = MIN(size, CDC_ACM_RX_SIZE - s->rx_count);
    first = MIN((uint32_t)size, CDC_ACM_RX_SIZE - tail);
    memcpy(s->rx_buf + tail, buf, first);
    if ((uint32_t)size > first) {
        memcpy(s->rx_buf, buf + first, size - first);
    }
    s->rx_count += size;
    cdc_acm_trace_bytes("chardev_rx", "host_to_guest", buf, size);
    usb_wakeup(s->bulk_in, 0);
    usb_wakeup(s->bulk_in_secondary, 0);
}

static void cdc_acm_event(void *opaque, QEMUChrEvent event)
{
    (void)opaque;
    if (event == CHR_EVENT_BREAK) {
        trace_usb_cdc_acm_event("chardev_break", 0, 0);
    }
}

static void cdc_acm_realize(USBDevice *dev, Error **errp)
{
    USBCDCACMState *s = USB_CDC_ACM(dev);

    (void)errp;
    usb_desc_init(dev);
    s->intr = usb_ep_get(dev, USB_TOKEN_IN, CDC_ACM_EP_INTERRUPT);
    s->intr_secondary =
        usb_ep_get(dev, USB_TOKEN_IN, CDC_ACM_EP_INTERRUPT_SECONDARY);
    s->bulk_in = usb_ep_get(dev, USB_TOKEN_IN, CDC_ACM_EP_BULK_IN);
    s->bulk_in_secondary =
        usb_ep_get(dev, USB_TOKEN_IN, CDC_ACM_EP_SECONDARY_BULK_IN);
    if (qemu_chr_fe_backend_connected(&s->chr)) {
        qemu_chr_fe_set_handlers(&s->chr, cdc_acm_can_read, cdc_acm_read,
                                 cdc_acm_event, NULL, s, NULL, true);
    }
    if (s->conf.peers.ncs[0]) {
        qemu_macaddr_default_if_unset(&s->conf.macaddr);
        s->nic = qemu_new_nic(&net_cdc_acm_info, &s->conf,
                              object_get_typename(OBJECT(s)), s->dev.qdev.id,
                              &s->dev.qdev.mem_reentrancy_guard, s);
        qemu_format_nic_info_str(qemu_get_queue(s->nic), s->conf.macaddr.a);
    }
    trace_usb_cdc_acm_event("attach", 0x1199, 0x68ab);
}

static void cdc_acm_unrealize(USBDevice *dev)
{
    USBCDCACMState *s = USB_CDC_ACM(dev);

    if (s->nic) {
        qemu_del_nic(s->nic);
        s->nic = NULL;
    }
}

static NetClientInfo net_cdc_acm_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .receive = cdc_acm_nic_receive,
};

static const VMStateDescription vmstate_usb_cdc_acm = {
    .name = "usb-cdc-acm",
    .unmigratable = 1,
};

static const Property cdc_acm_properties[] = {
    DEFINE_PROP_CHR("chardev", USBCDCACMState, chr),
    DEFINE_NIC_PROPERTIES(USBCDCACMState, conf),
};

static void cdc_acm_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    USBDeviceClass *uc = USB_DEVICE_CLASS(klass);

    (void)data;
    uc->realize = cdc_acm_realize;
    uc->unrealize = cdc_acm_unrealize;
    uc->handle_reset = cdc_acm_reset;
    uc->handle_control = cdc_acm_handle_control;
    uc->handle_data = cdc_acm_handle_data;
    uc->product_desc = "Sierra Wireless CDC ACM modem";
    uc->usb_desc = &cdc_acm_desc;
    dc->vmsd = &vmstate_usb_cdc_acm;
    device_class_set_props(dc, cdc_acm_properties);
    object_class_property_add(klass, "serial-state", "uint32",
                              cdc_acm_get_state, cdc_acm_set_state, NULL, NULL);
    object_class_property_set_description(
        klass, "serial-state",
        "CDC SERIAL_STATE bits: DCD bit 0 and DSR bit 1");
}

static const TypeInfo cdc_acm_type_info = {
    .name = TYPE_USB_CDC_ACM,
    .parent = TYPE_USB_DEVICE,
    .instance_size = sizeof(USBCDCACMState),
    .class_init = cdc_acm_class_init,
};

static void cdc_acm_register_types(void)
{
    type_register_static(&cdc_acm_type_info);
}

type_init(cdc_acm_register_types)
