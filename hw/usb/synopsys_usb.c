/*
 * Synopsys DesignWareCore for USB OTG.
 *
 * Copyright (c) 2011 Richard Ian Taylor.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "hw/hw.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/sysbus.h"
#include "hw/usb.h"
#include "hw/usb/tcp_usb.h"
#include "qemu/log.h"

#define GOTGCTL  0x0
#define GAHBCFG  0x8
#define GINTSTS  0x14
#define GINTMSK  0x18
#define GHWCFG1  0x44
#define GHWCFG2  0x48
#define GHWCFG3  0x4C
#define GHWCFG4  0x50
#define DCTL     0x804
#define DIEPMSK  0x810
#define DOEPMSK  0x814
#define DAINT    0x818
#define DAINTMSK 0x81C

#define INEP_BASE  0x900
#define OUTEP_BASE 0xB00
#define EP_SIZE    0x20
#define NUM_EP     5

#define DEPCTL  0x00
#define DEPINT  0x08
#define DEPTSIZ 0x10
#define DEPDMA  0x14

#define GOTGCTL_BSESVLD (1 << 19)

#define GAHBCFG_GLBLINTRMSK (1 << 0)
#define GAHBCFG_DMAEN       (1 << 5)

#define GINTMSK_GINNAKEFF  (1 << 6)
#define GINTMSK_GOUTNAKEFF (1 << 7)
#define GINTMSK_RESET      (1 << 12)
#define GINTMSK_ENUMDONE   (1 << 13)
#define GINTMSK_IEP        (1 << 18)
#define GINTMSK_OEP        (1 << 19)

#define GHWCFG2_NUMDEVEPS_SHIFT     10

#define DCTL_GNPINNAKSTS (1 << 2)
#define DCTL_GOUTNAKSTS  (1 << 3)
#define DCTL_SGNPINNAK   (1 << 7)
#define DCTL_CGNPINNAK   (1 << 8)
#define DCTL_SGOUTNAK    (1 << 9)
#define DCTL_CGOUTNAK    (1 << 10)

#define DEPCTL_NAKSTS (1 << 17)
#define DEPCTL_STALL  (1 << 21)
#define DEPCTL_CNAK   (1 << 26)
#define DEPCTL_SNAK   (1 << 27)
#define DEPCTL_EPDIS  (1 << 30)
#define DEPCTL_EPENA  (1 << 31)

#define DEPINT_XFERCOMPL  (1 << 0)
#define DEPINT_EPDISBLD   (1 << 1)
#define DEPINT_SETUP      (1 << 3)
#define DEPINT_INEPNAKEFF (1 << 6)

#define DEPTSIZ_SUPCNT_MASK   0x3
#define DEPTSIZ_SUPCNT_SHIFT  29
#define DEPTSIZ_XFERSIZ_MASK  0x7FFFF
#define DEPTSIZ_XFERSIZ_SHIFT 0

#define TYPE_SYNOPSYS_USB "synopsys_usb"
#define SYNOPSYS_USB(obj) OBJECT_CHECK(SynopsysUsbState, (obj), TYPE_SYNOPSYS_USB)

typedef struct SynopsysUsbEpState {
    uint32_t depctl;
    uint32_t depint;
    uint32_t deptsiz;
    hwaddr depdma;
} SynopsysUsbEpState;

typedef struct SynopsysUsbState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    qemu_irq irq;

    uint32_t port;
    TcpUsbState tcp_usb_state;

    uint32_t gotgctl;
    uint32_t gahbcfg;
    uint32_t gintsts;
    uint32_t gintmsk;
    uint32_t ghwcfg1;
    uint32_t ghwcfg2;
    uint32_t ghwcfg3;
    uint32_t ghwcfg4;
    uint32_t dctl;
    uint32_t diepmsk;
    uint32_t doepmsk;
    uint32_t daint;
    uint32_t daintmsk;

    SynopsysUsbEpState in_eps[NUM_EP];
    SynopsysUsbEpState out_eps[NUM_EP];
} SynopsysUsbState;

static void synopsys_usb_update_irq(SynopsysUsbState *s)
{
    int i;

    s->daint = 0;
    for (i = 0; i < NUM_EP; i++) {
        s->in_eps[i].depint &= ~DEPINT_INEPNAKEFF;
        if (s->in_eps[i].depctl & DEPCTL_NAKSTS) {
            s->in_eps[i].depint |= DEPINT_INEPNAKEFF;
        }

        if (s->out_eps[i].depint & s->doepmsk) {
            s->daint |= 1 << (i + 16);
        }
        if (s->in_eps[i].depint & s->diepmsk) {
            s->daint |= 1 << i;
        }
    }

    s->gintsts &= ~(GINTMSK_OEP | GINTMSK_IEP);
    if (s->daint & s->daintmsk & 0xffff0000) {
        s->gintsts |= GINTMSK_OEP;
    }
    if (s->daint & s->daintmsk & 0x0000ffff) {
        s->gintsts |= GINTMSK_IEP;
    }

    s->gintsts &= ~(GINTMSK_GINNAKEFF | GINTMSK_GOUTNAKEFF);
    if (s->dctl & DCTL_GNPINNAKSTS) {
        s->gintsts |= GINTMSK_GINNAKEFF;
    }
    if (s->dctl & DCTL_GOUTNAKSTS) {
        s->gintsts |= GINTMSK_GOUTNAKEFF;
    }

    if (s->gahbcfg & GAHBCFG_GLBLINTRMSK) {
        qemu_set_irq(s->irq, s->gintsts & s->gintmsk);
    } else {
        qemu_irq_lower(s->irq);
    }
}

static int synopsys_usb_tcp_callback(void *arg, const TcpUsbHeader *header, char *buffer)
{
    SynopsysUsbState *s = SYNOPSYS_USB(arg);
    uint8_t ep;
    SynopsysUsbEpState *eps;
    size_t sz, sup, count;

    if (header->flags & tcp_usb_reset) {
        s->gintsts |= GINTMSK_RESET;
        synopsys_usb_update_irq(s);
        return USB_RET_SUCCESS;
    }

    ep = header->ep & 0x7f;
    if (ep >= NUM_EP) {
        return USB_RET_NODEV;
    }
    eps = (header->ep & USB_DIR_IN) ? &s->in_eps[ep] : &s->out_eps[ep];

    if (header->length == 0) {
        return 0;
    }

    if (!(header->flags & tcp_usb_setup)) {
        if ((eps->depctl & DEPCTL_STALL)) {
            return USB_RET_STALL;
        }

        if ((eps->depctl & DEPCTL_NAKSTS) ||
                ((header->ep & USB_DIR_IN) && (s->dctl & DCTL_GNPINNAKSTS)) ||
                (!(header->ep & USB_DIR_IN) && (s->dctl & DCTL_GOUTNAKSTS))) {
            return USB_RET_NAK;
        }
    }

    if (!(eps->depctl & DEPCTL_EPENA)) {
        return USB_RET_NODEV;
    }

    eps->depctl &= ~(DEPCTL_EPENA | DEPCTL_STALL);

    sz = (eps->deptsiz >> DEPTSIZ_XFERSIZ_SHIFT) & DEPTSIZ_XFERSIZ_MASK;
    sup = (eps->deptsiz >> DEPTSIZ_SUPCNT_SHIFT) & DEPTSIZ_SUPCNT_MASK;

    count = header->length;
    if (!(header->flags & tcp_usb_setup) && count > sz) {
        count = sz;
    }

    if (s->gahbcfg & GAHBCFG_DMAEN) {
        if (header->ep & USB_DIR_IN) {
            cpu_physical_memory_read(eps->depdma, buffer, count);
        } else {
            cpu_physical_memory_write(eps->depdma, buffer, count);
        }
        eps->depdma += count;
    }

    if (header->flags & tcp_usb_setup) {
        sup--;
        eps->deptsiz &= ~(DEPTSIZ_SUPCNT_MASK << DEPTSIZ_SUPCNT_SHIFT);
        eps->deptsiz |= (sup & DEPTSIZ_SUPCNT_MASK) << DEPTSIZ_SUPCNT_SHIFT;

        eps->depint |= DEPINT_SETUP;
    } else {
        sz -= count;
        eps->deptsiz &= ~(DEPTSIZ_XFERSIZ_MASK << DEPTSIZ_XFERSIZ_SHIFT);
        eps->deptsiz |= (sz & DEPTSIZ_XFERSIZ_MASK) << DEPTSIZ_XFERSIZ_SHIFT;

        eps->depint |= DEPINT_XFERCOMPL;
    }

    synopsys_usb_update_irq(s);
    return count;
}

static uint64_t synopsys_usb_ep_read(SynopsysUsbState *s, SynopsysUsbEpState *eps, hwaddr offset, unsigned size)
{
    switch (offset) {
        case DEPCTL:
            return eps->depctl;

        case DEPINT:
            return eps->depint;

        case DEPTSIZ:
            return eps->deptsiz;

        case DEPDMA:
            return eps->depdma;

        default:
            qemu_log_mask(LOG_UNIMP, "%s: unimplemented ep read @ 0x%" HWADDR_PRIx "\n", __func__, offset);
            return 0;
    }
}

static void synopsys_usb_ep_write(SynopsysUsbState *s, SynopsysUsbEpState *eps, hwaddr offset, uint64_t value, unsigned size)
{
    switch (offset) {
        case DEPCTL:
            value &= ~DEPCTL_NAKSTS;
            value |= eps->depctl & (DEPCTL_NAKSTS | DEPCTL_EPENA);

            if (value & DEPCTL_EPDIS) {
                value &= ~DEPCTL_EPENA;
                eps->depint |= DEPINT_EPDISBLD;
            }

            if (value & DEPCTL_SNAK) {
                value |= DEPCTL_NAKSTS;
            } else if (value & DEPCTL_CNAK) {
                value &= ~DEPCTL_NAKSTS;
            }

            eps->depctl = value & ~(DEPCTL_EPDIS | DEPCTL_SNAK | DEPCTL_CNAK);
            synopsys_usb_update_irq(s);
            break;

        case DEPINT:
            eps->depint &= ~value;
            synopsys_usb_update_irq(s);
            break;

        case DEPTSIZ:
            eps->deptsiz = value;
            break;

        case DEPDMA:
            eps->depdma = value;
            break;

        default:
            qemu_log_mask(LOG_UNIMP, "%s: unimplemented ep write @ 0x%" HWADDR_PRIx ": 0x%" PRIx64 "\n", __func__, offset, value);
    }
}

static uint64_t synopsys_usb_read(void *arg, hwaddr offset, unsigned size)
{
    SynopsysUsbState *s = SYNOPSYS_USB(arg);

    if (offset >= INEP_BASE && offset < INEP_BASE + (NUM_EP * EP_SIZE)) {
        return synopsys_usb_ep_read(s, &s->in_eps[(offset - INEP_BASE) / EP_SIZE], offset & (EP_SIZE - 1), size);
    } else if (offset >= OUTEP_BASE && offset < OUTEP_BASE + (NUM_EP * EP_SIZE)) {
        return synopsys_usb_ep_read(s, &s->out_eps[(offset - OUTEP_BASE) / EP_SIZE], offset & (EP_SIZE - 1), size);
    } else {
        switch (offset) {
            case GOTGCTL:
                return s->gotgctl;

            case GAHBCFG:
                return s->gahbcfg;

            case GINTSTS:
                return s->gintsts;

            case GINTMSK:
                return s->gintmsk;

            case GHWCFG1:
                return s->ghwcfg1;

            case GHWCFG2:
                return s->ghwcfg2;

            case GHWCFG3:
                return s->ghwcfg3;

            case GHWCFG4:
                return s->ghwcfg4;

            case DCTL:
                return s->dctl;

            case DIEPMSK:
                return s->diepmsk;

            case DOEPMSK:
                return s->doepmsk;

            case DAINT:
                return s->daint;

            case DAINTMSK:
                return s->daintmsk;

            default:
                qemu_log_mask(LOG_UNIMP, "%s: unimplemented read @ 0x%" HWADDR_PRIx "\n", __func__, offset);
                return 0;
        }
    }
}

static void synopsys_usb_write(void *arg, hwaddr offset, uint64_t value, unsigned size)
{
    SynopsysUsbState *s = SYNOPSYS_USB(arg);

    if (offset >= INEP_BASE && offset < INEP_BASE + (NUM_EP * EP_SIZE)) {
        synopsys_usb_ep_write(s, &s->in_eps[(offset - INEP_BASE) / EP_SIZE], offset & (EP_SIZE - 1), value, size);
    } else if (offset >= OUTEP_BASE && offset < OUTEP_BASE + (NUM_EP * EP_SIZE)) {
        synopsys_usb_ep_write(s, &s->out_eps[(offset - OUTEP_BASE) / EP_SIZE], offset & (EP_SIZE - 1), value, size);
    } else {
        switch (offset) {
            case GOTGCTL:
                s->gotgctl = value;
                break;

            case GAHBCFG:
                s->gahbcfg = value;
                synopsys_usb_update_irq(s);
                break;

            case GINTSTS:
                if (value & GINTMSK_ENUMDONE) {
                    s->gintmsk &= ~GINTMSK_ENUMDONE;
                }
                s->gintsts &= ~value;
                synopsys_usb_update_irq(s);
                break;

            case GINTMSK:
                if (value & GINTMSK_ENUMDONE) {
                    s->gintsts |= GINTMSK_ENUMDONE;
                }
                if (value & GINTMSK_RESET) {
                    if (tcp_usb_serve(&s->tcp_usb_state, s->port) < 0) {
                        hw_error("%s: failed to start tcp_usb server\n", __func__);
                    }
                }
                s->gintmsk = value;
                synopsys_usb_update_irq(s);
                break;

            case DCTL:
                value &= ~(DCTL_GNPINNAKSTS | DCTL_GOUTNAKSTS);
                value |= s->dctl & (DCTL_GNPINNAKSTS | DCTL_GOUTNAKSTS);

                if (value & DCTL_SGNPINNAK) {
                    value |= DCTL_GNPINNAKSTS;
                } else if (value & DCTL_CGNPINNAK) {
                    value &= ~DCTL_GNPINNAKSTS;
                }

                if (value & DCTL_SGOUTNAK) {
                    value |= DCTL_GOUTNAKSTS;
                } else if (value & DCTL_CGOUTNAK) {
                    value &= ~DCTL_GOUTNAKSTS;
                }

                s->dctl = value & ~(DCTL_SGNPINNAK | DCTL_CGNPINNAK | DCTL_SGOUTNAK | DCTL_CGOUTNAK);
                synopsys_usb_update_irq(s);
                break;

            case DIEPMSK:
                s->diepmsk = value;
                synopsys_usb_update_irq(s);
                break;

            case DOEPMSK:
                s->doepmsk = value;
                synopsys_usb_update_irq(s);
                break;

            case DAINTMSK:
                s->daintmsk = value;
                synopsys_usb_update_irq(s);
                break;

            default:
                qemu_log_mask(LOG_UNIMP, "%s: unimplemented write @ 0x%" HWADDR_PRIx ": 0x%" PRIx64 "\n", __func__, offset, value);
        }
    }
}

static void synopsys_usb_reset_ep(SynopsysUsbEpState *eps)
{
    eps->depctl = 0;
    eps->depint = 0;
    eps->deptsiz = 0;
    eps->depdma = 0;
}

static void synopsys_usb_reset(DeviceState *dev)
{
    SynopsysUsbState *s = SYNOPSYS_USB(dev);
    int i;

    s->gotgctl = GOTGCTL_BSESVLD;
    s->gahbcfg = 0;
    s->gintsts = 0;
    s->gintmsk = 0;
    s->ghwcfg1 = 0;
    s->ghwcfg2 = (NUM_EP - 1) << GHWCFG2_NUMDEVEPS_SHIFT;
    s->ghwcfg3 = 0;
    s->ghwcfg4 = 0;
    s->dctl = 0;
    s->diepmsk = 0;
    s->doepmsk = 0;
    s->daint = 0;
    s->daintmsk = 0;

    for (i = 0; i < NUM_EP; i++) {
        synopsys_usb_reset_ep(&s->in_eps[i]);
        synopsys_usb_reset_ep(&s->out_eps[i]);
    }

    synopsys_usb_update_irq(s);
}

static const struct MemoryRegionOps synopsys_usb_ops = {
    .read = synopsys_usb_read,
    .write = synopsys_usb_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void synopsys_usb_realize(DeviceState *dev, Error **errp)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    SynopsysUsbState *s = SYNOPSYS_USB(dev);

    tcp_usb_init(&s->tcp_usb_state, synopsys_usb_tcp_callback, s);

    memory_region_init_io(&s->mmio, OBJECT(dev), &synopsys_usb_ops, s, TYPE_SYNOPSYS_USB, 0x40000);
    sysbus_init_mmio(sbd, &s->mmio);

    sysbus_init_irq(sbd, &s->irq);
}

static Property synopsys_usb_properties[] = {
    DEFINE_PROP_UINT32("port", SynopsysUsbState, port, 7642),
    DEFINE_PROP_END_OF_LIST(),
};

static void synopsys_usb_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = synopsys_usb_realize;
    dc->reset = synopsys_usb_reset;
    device_class_set_props(dc, synopsys_usb_properties);
}

static const TypeInfo synopsys_usb_info = {
    .name          = TYPE_SYNOPSYS_USB,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SynopsysUsbState),
    .class_init    = synopsys_usb_class_init,
};

static void synopsys_usb_register_type(void)
{
    type_register_static(&synopsys_usb_info);
}

type_init(synopsys_usb_register_type)
