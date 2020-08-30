/* Fujitsu USB Controller (USB20HDC) */
#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "hw/usb.h"
#include "hw/usb/tcp_usb.h"
#include "qapi/error.h"
#include "qemu/log.h"

#define F_USB20HDC_REGISTER_MODE     0x0004
#define F_USB20HDC_REGISTER_INTEN    0x0008
#define F_USB20HDC_REGISTER_INTS     0x000c
#define F_USB20HDC_REGISTER_EPCMD0   0x0040
#define F_USB20HDC_REGISTER_DEVC     0x0200
#define F_USB20HDC_REGISTER_DEVS     0x0204
#define F_USB20HDC_REGISTER_DMAC1    0x0400
#define F_USB20HDC_REGISTER_DMATCI1  0x0408
#define F_USB20HDC_REGISTER_DMATC1   0x040c
#define F_USB20HDC_REGISTER_DMAC2    0x0420
#define F_USB20HDC_REGISTER_DMATCI2  0x0428
#define F_USB20HDC_REGISTER_DMATC2   0x042c
#define F_USB20HDC_REGISTER_EPCTRL0  0x8000
#define F_USB20HDC_REGISTER_EPCONF0  0x8040
#define F_USB20HDC_REGISTER_EPCOUNT0 0x8080
#define F_USB20HDC_REGISTER_EPCOUNT1 0x8084

#define F_USB20HDC_HDMAC_REGISTER_DMACSA1 0x18
#define F_USB20HDC_HDMAC_REGISTER_DMACDA1 0x1c
#define F_USB20HDC_HDMAC_REGISTER_DMACSA2 0x28
#define F_USB20HDC_HDMAC_REGISTER_DMACDA2 0x2c

#define F_USB20HDC_NUM_EP 12
#define F_USB20HDC_NUM_DMA 2

#define F_USB20HDC_REGISTER_MODE_DEV_EN (1 << 1)

#define F_USB20HDC_REGISTER_INT_DEV (1 << 1)
#define F_USB20HDC_REGISTER_INT_DMA_SHIFT 8
#define F_USB20HDC_REGISTER_INT_EP_SHIFT 16

#define F_USB20HDC_REGISTER_EPCMD_START (1 << 0)
#define F_USB20HDC_REGISTER_EPCMD_STOP (1 << 1)
#define F_USB20HDC_REGISTER_EPCMD_STALL_SET (1 << 5)
#define F_USB20HDC_REGISTER_EPCMD_STALL_CLEAR (1 << 6)
#define F_USB20HDC_REGISTER_EPCMD_NACKRESP (1 << 10)
#define F_USB20HDC_REGISTER_EPCMD_READYI_READY_INTEN (1 << 12)
#define F_USB20HDC_REGISTER_EPCMD_READYO_EMPTY_INTEN (1 << 13)
#define F_USB20HDC_REGISTER_EPCMD_READYI_READY_INT_CLR (1 << 18)
#define F_USB20HDC_REGISTER_EPCMD_READYO_EMPTY_INT_CLR (1 << 19)

#define F_USB20HDC_REGISTER_DEVS_SUSPEND (1 << 0)

#define F_USB20HDC_REGISTER_DEV_INT_SETUP (1 << 27)
#define F_USB20HDC_REGISTER_DEV_INT_USBRSTE (1 << 28)
#define F_USB20HDC_REGISTER_DEV_INT_USBRSTB (1 << 29)

#define F_USB20HDC_REGISTER_DMAC_START (1 << 0)
#define F_USB20HDC_REGISTER_DMAC_SENDNULL (1 << 3)
#define F_USB20HDC_REGISTER_DMAC_EP_SHIFT 8
#define F_USB20HDC_REGISTER_DMAC_EP_MASK 0xf

#define F_USB20HDC_REGISTER_EPCTRL_EN (1 << 0)
#define F_USB20HDC_REGISTER_EPCTRL_STALL (1 << 12)
#define F_USB20HDC_REGISTER_EPCTRL_NACKRESP (1 << 17)
#define F_USB20HDC_REGISTER_EPCTRL_READYI_READY_INTEN (1 << 18)
#define F_USB20HDC_REGISTER_EPCTRL_READYO_EMPTY_INTEN (1 << 19)
#define F_USB20HDC_REGISTER_EPCTRL_READYI_READY_INT (1 << 26)
#define F_USB20HDC_REGISTER_EPCTRL_READYO_EMPTY_INT (1 << 27)

#define F_USB20HDC_REGISTER_EPCONF_BASE_SHIFT 0
#define F_USB20HDC_REGISTER_EPCONF_BASE_MASK 0x1fff
#define F_USB20HDC_REGISTER_EPCONF_SIZE_SHIFT 13
#define F_USB20HDC_REGISTER_EPCONF_SIZE_MASK 0x7ff

#define F_USB20HDC_REGISTER_EPCOUNT_APPCNT_SHIFT 0
#define F_USB20HDC_REGISTER_EPCOUNT_APPCNT_MASK 0x7ff
#define F_USB20HDC_REGISTER_EPCOUNT_PHYCNT_SHIFT 16
#define F_USB20HDC_REGISTER_EPCOUNT_PHYCNT_MASK 0x7ff

#define TYPE_FUJITSU_USB "fujitsu_usb"
#define FUJITSU_USB(obj) OBJECT_CHECK(FujitsuUsbState, (obj), TYPE_FUJITSU_USB)

typedef struct FujitsuUsbState {
    SysBusDevice parent_obj;
    MemoryRegion container;
    MemoryRegion mmio;
    MemoryRegion ram;
    MemoryRegion hdmac;
    qemu_irq intr;

    uint32_t port;
    TcpUsbState tcp_usb_state;

    uint32_t reg_inten;
    uint32_t reg_devc;
    uint32_t reg_devs;
    uint8_t reg_dmaint;

    uint32_t reg_epctrl[F_USB20HDC_NUM_EP];
    uint32_t reg_epconf[F_USB20HDC_NUM_EP];
    uint32_t reg_epcount0[F_USB20HDC_NUM_EP];
    uint32_t reg_epcount1[F_USB20HDC_NUM_EP];

    uint32_t reg_dmac[F_USB20HDC_NUM_DMA];
    uint32_t reg_dmatci[F_USB20HDC_NUM_DMA];
    uint32_t reg_dmatc[F_USB20HDC_NUM_DMA];
    uint32_t reg_dmacsa[F_USB20HDC_NUM_DMA];
    uint32_t reg_dmacda[F_USB20HDC_NUM_DMA];
} FujitsuUsbState;

static uint32_t fujitsu_usb_get_ints(FujitsuUsbState *s)
{
    int ep;
    uint32_t ints = s->reg_dmaint << F_USB20HDC_REGISTER_INT_DMA_SHIFT;

    if (s->reg_devc & s->reg_devs & (F_USB20HDC_REGISTER_DEV_INT_SETUP | F_USB20HDC_REGISTER_DEV_INT_USBRSTE | F_USB20HDC_REGISTER_DEV_INT_USBRSTB)) {
        ints |= F_USB20HDC_REGISTER_INT_DEV;
    }

    for (ep = 0; ep < F_USB20HDC_NUM_EP; ep++) {
        if (((s->reg_epctrl[ep] & F_USB20HDC_REGISTER_EPCTRL_READYI_READY_INTEN) && (s->reg_epctrl[ep] & F_USB20HDC_REGISTER_EPCTRL_READYI_READY_INT)) ||
            ((s->reg_epctrl[ep] & F_USB20HDC_REGISTER_EPCTRL_READYO_EMPTY_INTEN) && (s->reg_epctrl[ep] & F_USB20HDC_REGISTER_EPCTRL_READYO_EMPTY_INT))
        ) {
            ints |= 1 << (ep + F_USB20HDC_REGISTER_INT_EP_SHIFT);
        }
    }

    return ints;
}

static void fujitsu_usb_update_irq(FujitsuUsbState *s)
{
    qemu_set_irq(s->intr, s->reg_inten & fujitsu_usb_get_ints(s));
}

static int fujitsu_usb_tcp_callback(void *arg, const TcpUsbHeader *header, char *buffer)
{
    FujitsuUsbState *s = FUJITSU_USB(arg);
    int i;
    uint8_t ep;
    size_t count;
    uint32_t ep_base, ep_size, appcnt;
    void *ram = memory_region_get_ram_ptr(&s->ram);

    if (header->flags & tcp_usb_reset) {
        s->reg_devs |= F_USB20HDC_REGISTER_DEV_INT_USBRSTE | F_USB20HDC_REGISTER_DEV_INT_USBRSTB;
        fujitsu_usb_update_irq(s);
        return USB_RET_SUCCESS;
    }

    ep = header->ep & 0x7f;
    if (ep >= F_USB20HDC_NUM_EP) {
        return USB_RET_NODEV;
    }

    count = header->length;
    if (count == 0) {
        return 0;
    }

    if (!(s->reg_epctrl[ep] & F_USB20HDC_REGISTER_EPCTRL_EN)) {
        return USB_RET_NAK;
    }

    if (ep == 0) {
        ep_base = ((s->reg_epconf[ep] >> F_USB20HDC_REGISTER_EPCONF_BASE_SHIFT) & F_USB20HDC_REGISTER_EPCONF_BASE_MASK) * 4 - 0x100;
        ep_size = (s->reg_epconf[ep] >> F_USB20HDC_REGISTER_EPCONF_SIZE_SHIFT) & F_USB20HDC_REGISTER_EPCONF_SIZE_MASK;

        if (count > ep_size) {
            count = ep_size;
        }

        if (header->flags & tcp_usb_setup) {
            memcpy(ram + ep_base + ep_size, buffer, count);
            s->reg_epcount1[ep] = (count & F_USB20HDC_REGISTER_EPCOUNT_PHYCNT_MASK) << F_USB20HDC_REGISTER_EPCOUNT_PHYCNT_SHIFT;
            s->reg_devs |= F_USB20HDC_REGISTER_DEV_INT_SETUP;
        } else {
            if (header->ep & USB_DIR_IN) {
                appcnt = (s->reg_epcount0[ep] >> F_USB20HDC_REGISTER_EPCOUNT_APPCNT_SHIFT) & F_USB20HDC_REGISTER_EPCOUNT_APPCNT_MASK;

                if (count > appcnt) {
                    count = appcnt;
                }

                memcpy(buffer, ram + ep_base, count);
                s->reg_epcount0[ep] = ((appcnt - count) & F_USB20HDC_REGISTER_EPCOUNT_APPCNT_MASK) << F_USB20HDC_REGISTER_EPCOUNT_APPCNT_SHIFT;

                if (count == appcnt) {
                    s->reg_epctrl[ep] |= F_USB20HDC_REGISTER_EPCTRL_READYO_EMPTY_INT;
                }
            } else {
                if (count != 0) {
                    hw_error("%s: ep0 out transfer not supported\n", __func__);
                }
            }
        }

        fujitsu_usb_update_irq(s);
        return count;
    } else {
        if (s->reg_epctrl[ep] & F_USB20HDC_REGISTER_EPCTRL_NACKRESP) {
            return USB_RET_NAK;
        }

        if (s->reg_epctrl[ep] & F_USB20HDC_REGISTER_EPCTRL_STALL) {
            return USB_RET_STALL;
        }

        for (i = 0; i < F_USB20HDC_NUM_DMA; i++) {
            if ((s->reg_dmac[i] & F_USB20HDC_REGISTER_DMAC_START) && ((s->reg_dmac[i] >> F_USB20HDC_REGISTER_DMAC_EP_SHIFT) & F_USB20HDC_REGISTER_DMAC_EP_MASK) == ep) {
                if (s->reg_dmac[i] & F_USB20HDC_REGISTER_DMAC_SENDNULL) {
                    hw_error("%s: dma null transfer not supported\n", __func__);
                }

                if (count > s->reg_dmatci[i]) {
                    count = s->reg_dmatci[i];
                }

                if (header->ep & USB_DIR_IN) {
                    cpu_physical_memory_read(s->reg_dmacsa[i], buffer, count);
                } else {
                    cpu_physical_memory_write(s->reg_dmacda[i], buffer, count);
                }

                s->reg_dmac[i] &= ~F_USB20HDC_REGISTER_DMAC_START;
                s->reg_dmatc[i] = count;
                s->reg_dmaint |= 1 << i;

                fujitsu_usb_update_irq(s);
                return count;
            }
        }

        return USB_RET_NAK;
    }
}

static uint64_t fujitsu_usb_read(void *arg, hwaddr offset, unsigned size)
{
    FujitsuUsbState *s = FUJITSU_USB(arg);
    uint8_t ep;

    switch (offset) {
        case F_USB20HDC_REGISTER_INTEN:
            return s->reg_inten;

        case F_USB20HDC_REGISTER_INTS:
            return fujitsu_usb_get_ints(s);

        case F_USB20HDC_REGISTER_DEVC:
            return s->reg_devc;

        case F_USB20HDC_REGISTER_DEVS:
            return s->reg_devs;

        case F_USB20HDC_REGISTER_DMAC1:
            return s->reg_dmac[0];

        case F_USB20HDC_REGISTER_DMATCI1:
            return s->reg_dmatci[0];

        case F_USB20HDC_REGISTER_DMATC1:
            return s->reg_dmatc[0];

        case F_USB20HDC_REGISTER_DMAC2:
            return s->reg_dmac[1];

        case F_USB20HDC_REGISTER_DMATCI2:
            return s->reg_dmatci[1];

        case F_USB20HDC_REGISTER_DMATC2:
            return s->reg_dmatc[1];

        case F_USB20HDC_REGISTER_EPCTRL0 ... F_USB20HDC_REGISTER_EPCTRL0 + (F_USB20HDC_NUM_EP - 1) * 4:
            ep = (offset - F_USB20HDC_REGISTER_EPCTRL0) >> 2;
            return s->reg_epctrl[ep];

        case F_USB20HDC_REGISTER_EPCONF0 ... F_USB20HDC_REGISTER_EPCONF0 + (F_USB20HDC_NUM_EP - 1) * 4:
            ep = (offset - F_USB20HDC_REGISTER_EPCONF0) >> 2;
            return s->reg_epconf[ep];

        case F_USB20HDC_REGISTER_EPCOUNT0 ... F_USB20HDC_REGISTER_EPCOUNT0 + (F_USB20HDC_NUM_EP - 1) * 8:
            ep = (offset - F_USB20HDC_REGISTER_EPCOUNT0) >> 3;
            return (((offset - F_USB20HDC_REGISTER_EPCOUNT0) >> 2) & 1) ? s->reg_epcount1[ep] : s->reg_epcount0[ep];

        default:
            qemu_log_mask(LOG_UNIMP, "%s: unimplemented read @ 0x%" HWADDR_PRIx "\n", __func__, offset);
            return 0;
    }
}

static void fujitsu_usb_write(void *arg, hwaddr offset, uint64_t value, unsigned size)
{
    FujitsuUsbState *s = FUJITSU_USB(arg);
    uint8_t ep;

    switch (offset) {
        case F_USB20HDC_REGISTER_MODE:
            if (value & F_USB20HDC_REGISTER_MODE_DEV_EN) {
                s->reg_devs |= F_USB20HDC_REGISTER_DEVS_SUSPEND;
                fujitsu_usb_update_irq(s);
            }
            break;

        case F_USB20HDC_REGISTER_INTEN:
            s->reg_inten = value;
            fujitsu_usb_update_irq(s);
            break;

        case F_USB20HDC_REGISTER_INTS:
            s->reg_dmaint &= value >> F_USB20HDC_REGISTER_INT_DMA_SHIFT;
            fujitsu_usb_update_irq(s);
            break;

        case F_USB20HDC_REGISTER_EPCMD0 ... F_USB20HDC_REGISTER_EPCMD0 + (F_USB20HDC_NUM_EP - 1) * 4:
            ep = (offset - F_USB20HDC_REGISTER_EPCMD0) >> 2;

            if (value & F_USB20HDC_REGISTER_EPCMD_START) {
                s->reg_epctrl[ep] |= F_USB20HDC_REGISTER_EPCTRL_EN;
            }
            if (value & F_USB20HDC_REGISTER_EPCMD_STOP) {
                s->reg_epctrl[ep] &= ~F_USB20HDC_REGISTER_EPCTRL_EN;
            }

            if (value & F_USB20HDC_REGISTER_EPCMD_STALL_SET) {
                s->reg_epctrl[ep] |= F_USB20HDC_REGISTER_EPCTRL_STALL;
            }
            if (value & F_USB20HDC_REGISTER_EPCMD_STALL_CLEAR) {
                s->reg_epctrl[ep] &= ~F_USB20HDC_REGISTER_EPCTRL_STALL;
            }

            if (value & F_USB20HDC_REGISTER_EPCMD_NACKRESP) {
                s->reg_epctrl[ep] |= F_USB20HDC_REGISTER_EPCTRL_NACKRESP;
            } else {
                s->reg_epctrl[ep] &= ~F_USB20HDC_REGISTER_EPCTRL_NACKRESP;
            }

            if (value & F_USB20HDC_REGISTER_EPCMD_READYI_READY_INTEN) {
                s->reg_epctrl[ep] |= F_USB20HDC_REGISTER_EPCTRL_READYI_READY_INTEN;
            } else {
                s->reg_epctrl[ep] &= ~F_USB20HDC_REGISTER_EPCTRL_READYI_READY_INTEN;
            }

            if (value & F_USB20HDC_REGISTER_EPCMD_READYO_EMPTY_INTEN) {
                s->reg_epctrl[ep] |= F_USB20HDC_REGISTER_EPCTRL_READYO_EMPTY_INTEN;
            } else {
                s->reg_epctrl[ep] &= ~F_USB20HDC_REGISTER_EPCTRL_READYO_EMPTY_INTEN;
            }

            if (value & F_USB20HDC_REGISTER_EPCMD_READYI_READY_INT_CLR) {
                s->reg_epctrl[ep] &= ~F_USB20HDC_REGISTER_EPCTRL_READYI_READY_INT;
            }

            if (value & F_USB20HDC_REGISTER_EPCMD_READYO_EMPTY_INT_CLR) {
                s->reg_epctrl[ep] &= ~F_USB20HDC_REGISTER_EPCTRL_READYO_EMPTY_INT;
            }

            fujitsu_usb_update_irq(s);
            break;

        case F_USB20HDC_REGISTER_DEVC:
            s->reg_devc = value;
            if ((value & F_USB20HDC_REGISTER_DEV_INT_USBRSTE) && (value & F_USB20HDC_REGISTER_DEV_INT_USBRSTB)) {
                if (tcp_usb_serve(&s->tcp_usb_state, s->port) < 0) {
                    hw_error("%s: failed to start tcp_usb server\n", __func__);
                }
            }
            fujitsu_usb_update_irq(s);
            break;

        case F_USB20HDC_REGISTER_DEVS:
            s->reg_devs &= value;
            fujitsu_usb_update_irq(s);
            break;

        case F_USB20HDC_REGISTER_DMAC1:
            s->reg_dmac[0] = value;
            break;

        case F_USB20HDC_REGISTER_DMATCI1:
            s->reg_dmatci[0] = value;
            break;

        case F_USB20HDC_REGISTER_DMAC2:
            s->reg_dmac[1] = value;
            break;

        case F_USB20HDC_REGISTER_DMATCI2:
            s->reg_dmatci[1] = value;
            break;

        case F_USB20HDC_REGISTER_EPCONF0 ... F_USB20HDC_REGISTER_EPCONF0 + (F_USB20HDC_NUM_EP - 1) * 4:
            ep = (offset - F_USB20HDC_REGISTER_EPCONF0) >> 2;
            s->reg_epconf[ep] = value;
            break;

        case F_USB20HDC_REGISTER_EPCOUNT0 ... F_USB20HDC_REGISTER_EPCOUNT0 + (F_USB20HDC_NUM_EP - 1) * 8:
            ep = (offset - F_USB20HDC_REGISTER_EPCOUNT0) >> 3;
            if (((offset - F_USB20HDC_REGISTER_EPCOUNT0) >> 2) & 1) {
                s->reg_epcount1[ep] = value;
            } else {
                s->reg_epcount0[ep] = value;
            }
            break;

        default:
            qemu_log_mask(LOG_UNIMP, "%s: unimplemented write @ 0x%" HWADDR_PRIx ": 0x%" PRIx64 "\n", __func__, offset, value);
    }
}

static const struct MemoryRegionOps fujitsu_usb_ops = {
    .read = fujitsu_usb_read,
    .write = fujitsu_usb_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static uint64_t fujitsu_usb_hdmac_read(void *arg, hwaddr offset, unsigned size)
{
    qemu_log_mask(LOG_UNIMP, "%s: unimplemented read @ 0x%" HWADDR_PRIx "\n", __func__, offset);
    return 0;
}

static void fujitsu_usb_hdmac_write(void *arg, hwaddr offset, uint64_t value, unsigned size)
{
    FujitsuUsbState *s = FUJITSU_USB(arg);

    switch (offset) {
        case F_USB20HDC_HDMAC_REGISTER_DMACSA1:
            s->reg_dmacsa[0] = value;
            break;

        case F_USB20HDC_HDMAC_REGISTER_DMACDA1:
            s->reg_dmacda[0] = value;
            break;

        case F_USB20HDC_HDMAC_REGISTER_DMACSA2:
            s->reg_dmacsa[1] = value;
            break;

        case F_USB20HDC_HDMAC_REGISTER_DMACDA2:
            s->reg_dmacda[1] = value;
            break;

        default:
            qemu_log_mask(LOG_UNIMP, "%s: unimplemented write @ 0x%" HWADDR_PRIx ": 0x%" PRIx64 "\n", __func__, offset, value);
    }
}

static const struct MemoryRegionOps fujitsu_usb_hdmac_ops = {
    .read = fujitsu_usb_hdmac_read,
    .write = fujitsu_usb_hdmac_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void fujitsu_usb_reset(DeviceState *dev)
{
    FujitsuUsbState *s = FUJITSU_USB(dev);
    int i;

    s->reg_inten = 0;
    s->reg_devc = 0;
    s->reg_devs = 0;
    s->reg_dmaint = 0;

    for (i = 0; i < F_USB20HDC_NUM_EP; i++) {
        s->reg_epctrl[i] = 0;
        s->reg_epconf[i] = 0;
        s->reg_epcount0[i] = 0;
        s->reg_epcount1[i] = 0;
    }

    for (i = 0; i < F_USB20HDC_NUM_DMA; i++) {
        s->reg_dmac[i] = 0;
        s->reg_dmatci[i] = 0;
        s->reg_dmatc[i] = 0;
        s->reg_dmacsa[i] = 0;
        s->reg_dmacda[i] = 0;
    }

}

static int fujitsu_usb_init(SysBusDevice *dev)
{
    FujitsuUsbState *s = FUJITSU_USB(dev);

    tcp_usb_init(&s->tcp_usb_state, fujitsu_usb_tcp_callback, s);

    memory_region_init(&s->container, OBJECT(dev), TYPE_FUJITSU_USB ".container", 0x10000);
    sysbus_init_mmio(dev, &s->container);

    memory_region_init_io(&s->mmio, OBJECT(dev), &fujitsu_usb_ops, s, TYPE_FUJITSU_USB ".mmio", 0x8100);
    memory_region_add_subregion(&s->container, 0, &s->mmio);

    memory_region_init_ram(&s->ram, OBJECT(dev), TYPE_FUJITSU_USB ".epbuf", 0x7f00, &error_fatal);
    memory_region_add_subregion(&s->container, 0x8100, &s->ram);

    memory_region_init_io(&s->hdmac, OBJECT(dev), &fujitsu_usb_hdmac_ops, s, TYPE_FUJITSU_USB ".hdmac", 0x100);
    sysbus_init_mmio(dev, &s->hdmac);

    sysbus_init_irq(dev, &s->intr);
    return 0;
}

static Property fujitsu_usb_properties[] = {
    DEFINE_PROP_UINT32("port", FujitsuUsbState, port, 7642),
    DEFINE_PROP_END_OF_LIST(),
};

static void fujitsu_usb_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SysBusDeviceClass *k = SYS_BUS_DEVICE_CLASS(klass);

    k->init = fujitsu_usb_init;
    dc->reset = fujitsu_usb_reset;
    dc->props = fujitsu_usb_properties;
}

static const TypeInfo fujitsu_usb_info = {
    .name          = TYPE_FUJITSU_USB,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(FujitsuUsbState),
    .class_init    = fujitsu_usb_class_init,
};

static void fujitsu_usb_register_type(void)
{
    type_register_static(&fujitsu_usb_info);
}

type_init(fujitsu_usb_register_type)
