/* Mentor Graphics Inventra Dual-Role USB Controller (MUSBMHDRC) */
#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "hw/usb.h"
#include "hw/usb/tcp_usb.h"
#include "qemu/log.h"

#define INTRTX    0x02
#define INTRRX    0x04
#define INTRTXE   0x06
#define INTRRXE   0x08
#define INTRUSB   0x0a
#define INTRUSBE  0x0b
#define INDEX     0x0e
#define DMA_INTR  0x200
#define DMA_CNTL  0x204
#define DMA_ADDR  0x208
#define DMA_COUNT 0x20c

#define EP_BASE   0x10
#define EP_SIZE   0x10
#define NUM_EP    7
#define FIFO_BASE 0x20
#define FIFO_SIZE 64

#define CSR0   0x02
#define COUNT0 0x08

#define TXMAXP  0x00
#define TXCSR   0x02
#define RXCSR   0x06
#define RXCOUNT 0x08

#define INTRUSB_RESET 0x04

#define CSR0_RXPKTRDY    0x0001
#define CSR0_TXPKTRDY    0x0002
#define CSR0_SENTSTALL   0x0004
#define CSR0_SENDSTALL   0x0020
#define CSR0_SVDRXPKTRDY 0x0040
#define CSR0_FLUSHFIFO   0x0100

#define TXCSRL_TXPKTRDY  0x01
#define TXCSRL_FLUSHFIFO 0x08
#define TXCSRL_SENDSTALL 0x10
#define TXCSRL_SENTSTALL 0x20

#define RXCSRL_RXPKTRDY  0x01
#define RXCSRL_FLUSHFIFO 0x10
#define RXCSRL_SENDSTALL 0x20
#define RXCSRL_SENTSTALL 0x40

#define DMA_ENAB     0x01
#define DMA_DIR      0x02
#define DMA_IE       0x08
#define DMA_EP_SHIFT 0x04
#define DMA_EP_MASK  0x0f

#define TYPE_INVENTRA_USB "inventra_usb"
#define INVENTRA_USB(obj) OBJECT_CHECK(InventraUsbState, (obj), TYPE_INVENTRA_USB)

typedef struct FifoState {
    char buf[FIFO_SIZE];
    size_t r;
    size_t w;
} FifoState;

typedef struct InventraUsbEpState {
    uint16_t txmaxp;
    uint8_t txcsrl;
    uint8_t txcsrh;
    uint8_t rxcsrl;
    uint8_t rxcsrh;
    FifoState txfifo;
    FifoState rxfifo;
} InventraUsbEpState;

typedef struct InventraUsbState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    qemu_irq intr0;
    qemu_irq intr1;

    uint32_t port;
    TcpUsbState tcp_usb_state;

    uint8_t intrusb;
    uint8_t intrusbe;
    uint16_t intrtx;
    uint16_t intrtxe;
    uint16_t intrrx;
    uint16_t intrrxe;
    uint8_t index;

    uint8_t dma_intr;
    uint16_t dma_cntl;
    uint32_t dma_addr;
    uint32_t dma_count;

    uint16_t csr0;
    FifoState fifo0;

    InventraUsbEpState eps[NUM_EP - 1];
} InventraUsbState;

static void fifo_flush(FifoState *fifo)
{
    fifo->r = 0;
    fifo->w = 0;
}

static size_t fifo_count(FifoState *fifo)
{
    return fifo->w - fifo->r;
}

static void fifo_read(FifoState *fifo, char *buf, size_t len)
{
    if (fifo->r + len > fifo->w) {
        hw_error("%s: out of data\n", __func__);
    }
    memcpy(buf, fifo->buf + fifo->r, len);
    fifo->r += len;
    if (fifo_count(fifo) == 0) {
        fifo_flush(fifo);
    }
}

static void fifo_write(FifoState *fifo, const char *buf, size_t len)
{
    if (fifo->w + len > sizeof(fifo->buf)) {
        hw_error("%s: out of capacity\n", __func__);
    }
    memcpy(fifo->buf + fifo->w, buf, len);
    fifo->w += len;
}

static void inventra_usb_update_irq(InventraUsbState *s)
{
    qemu_set_irq(s->intr0, (s->intrusb & s->intrusbe) | (s->intrtx & s->intrtxe) | (s->intrrx & s->intrrxe));
    qemu_set_irq(s->intr1, (s->dma_cntl & DMA_IE) ? s->dma_intr : 0);
}

static int inventra_usb_tcp_callback(void *arg, const TcpUsbHeader *header, char *buffer)
{
    InventraUsbState *s = INVENTRA_USB(arg);
    uint8_t ep;
    InventraUsbEpState *eps;
    size_t count, residue;
    bool dma_en;
    char dma_buf[FIFO_SIZE];

    if (header->flags & tcp_usb_reset) {
        s->intrusb |= INTRUSB_RESET;
        inventra_usb_update_irq(s);
        return USB_RET_SUCCESS;
    }

    ep = header->ep & 0x7f;
    if (ep >= NUM_EP) {
        return USB_RET_NODEV;
    }

    count = header->length;

    if (ep == 0) {
        if (s->csr0 & CSR0_SENDSTALL) {
            s->csr0 &= ~CSR0_SENDSTALL;
            s->csr0 |= CSR0_SENTSTALL;
            s->intrtx |= 1 << ep;
            inventra_usb_update_irq(s);
            return USB_RET_NAK;
        }

        if (header->ep & USB_DIR_IN) {
            if (!(s->csr0 & CSR0_TXPKTRDY)) {
                return USB_RET_NAK;
            }
            fifo_read(&s->fifo0, buffer, count);
            s->csr0 &= ~CSR0_TXPKTRDY;
        } else {
            fifo_write(&s->fifo0, buffer, count);
            s->csr0 |= CSR0_RXPKTRDY;
        }
        s->intrtx |= 1 << ep;
    } else {
        eps = &s->eps[ep - 1];
        dma_en = (s->dma_cntl & DMA_ENAB) &&
                 (((s->dma_cntl >> DMA_EP_SHIFT) & DMA_EP_MASK) == ep) &&
                 !(s->dma_cntl & DMA_DIR) == !(header->ep & USB_DIR_IN);

        if (header->ep & USB_DIR_IN) {
            if (eps->txcsrl & TXCSRL_SENDSTALL) {
                eps->txcsrl &= ~TXCSRL_SENDSTALL;
                eps->txcsrl |= TXCSRL_SENTSTALL;
                s->intrtx |= 1 << ep;
                inventra_usb_update_irq(s);
                return USB_RET_STALL;
            }

            if (dma_en) {
                if (count < s->dma_count) {
                    hw_error("%s: packet length less than dma count\n", __func__);
                }
                residue = s->dma_count % eps->txmaxp;
                count = s->dma_count - residue;
                cpu_physical_memory_read(s->dma_addr, buffer, count);
                s->dma_addr += count;

                if (residue > sizeof(dma_buf)) {
                    hw_error("%s: out of capacity for residue\n", __func__);
                }
                cpu_physical_memory_read(s->dma_addr, dma_buf, residue);
                fifo_write(&eps->txfifo, dma_buf, residue);
                s->dma_intr |= 1;
            } else {
                if (!(eps->txcsrl & TXCSRL_TXPKTRDY)) {
                    return USB_RET_NAK;
                }
                fifo_read(&eps->txfifo, buffer, count);
                eps->txcsrl &= ~TXCSRL_TXPKTRDY;
                s->intrtx |= 1 << ep;
            }
        } else {
            if (eps->rxcsrl & RXCSRL_SENDSTALL) {
                eps->rxcsrl &= ~RXCSRL_SENDSTALL;
                eps->rxcsrl |= RXCSRL_SENTSTALL;
                s->intrrx |= 1 << ep;
                inventra_usb_update_irq(s);
                return USB_RET_STALL;
            }

            if (dma_en) {
                if (count < s->dma_count) {
                    hw_error("%s: packet length less than dma count\n", __func__);
                }
                cpu_physical_memory_write(s->dma_addr, buffer, s->dma_count);
                s->dma_addr += s->dma_count;
                s->dma_intr |= 1;
                fifo_write(&eps->rxfifo, buffer + s->dma_count, count - s->dma_count);
            } else {
                fifo_write(&eps->rxfifo, buffer, count);
            }

            if (fifo_count(&eps->rxfifo) > 0) {
                eps->rxcsrl |= RXCSRL_RXPKTRDY;
                s->intrrx |= 1 << ep;
            }
        }
    }

    inventra_usb_update_irq(s);
    return count;
}

static uint64_t inventra_usb_ep0_read(InventraUsbState *s, hwaddr offset, unsigned size)
{
    if (size == 1) {
        switch (offset) {
            case COUNT0:
                return fifo_count(&s->fifo0);
        }
    } else if (size == 2) {
        switch (offset) {
            case CSR0:
                return s->csr0;
        }
    }

    qemu_log_mask(LOG_UNIMP, "%s: unimplemented ep0 read @ 0x%" HWADDR_PRIx "\n", __func__, offset);
    return 0;
}

static void inventra_usb_ep0_write(InventraUsbState *s, hwaddr offset, uint64_t value, unsigned size)
{
    if (size == 2) {
        switch (offset) {
            case CSR0:
                if (value & CSR0_FLUSHFIFO) {
                    fifo_flush(&s->fifo0);
                }
                value &= ~CSR0_RXPKTRDY;
                if ((s->csr0 & CSR0_RXPKTRDY) && !(value & CSR0_SVDRXPKTRDY)) {
                    value |= CSR0_RXPKTRDY;
                }
                s->csr0 = value & (CSR0_RXPKTRDY | CSR0_TXPKTRDY | CSR0_SENDSTALL | (s->csr0 & CSR0_SENTSTALL));
                return;
        }
    }

    qemu_log_mask(LOG_UNIMP, "%s: unimplemented ep0 write @ 0x%" HWADDR_PRIx ": 0x%" PRIx64 "\n", __func__, offset, value);
}

static uint64_t inventra_usb_ep_read(InventraUsbState *s, InventraUsbEpState *eps, hwaddr offset, unsigned size)
{
    if (size == 2) {
        switch (offset) {
            case TXCSR:
                return (eps->txcsrh << 8) | eps->txcsrl;

            case RXCSR:
                return (eps->rxcsrh << 8) | eps->rxcsrl;

            case RXCOUNT:
                return fifo_count(&eps->rxfifo);
        }
    }

    qemu_log_mask(LOG_UNIMP, "%s: unimplemented ep read @ 0x%" HWADDR_PRIx "\n", __func__, offset);
    return 0;
}

static void inventra_usb_ep_write(InventraUsbState *s, InventraUsbEpState *eps, hwaddr offset, uint64_t value, unsigned size)
{
    if (size == 1) {
        switch (offset) {
            case TXCSR:
                if (value & TXCSRL_FLUSHFIFO) {
                    fifo_flush(&eps->txfifo);
                }
                eps->txcsrl = value & (TXCSRL_TXPKTRDY | TXCSRL_SENDSTALL | (eps->txcsrl & TXCSRL_SENTSTALL));
                return;

            case TXCSR + 1:
                eps->txcsrh = value;
                return;

            case RXCSR:
                if (value & RXCSRL_FLUSHFIFO) {
                    fifo_flush(&eps->rxfifo);
                }
                eps->rxcsrl = value & (RXCSRL_RXPKTRDY | RXCSRL_SENDSTALL | (eps->rxcsrl & RXCSRL_SENTSTALL));
                return;

            case RXCSR + 1:
                eps->rxcsrh = value;
                return;
        }
    } else if (size == 2) {
        switch (offset) {
            case TXMAXP:
                eps->txmaxp = value;
                return;

            case TXCSR:
            case RXCSR:
                inventra_usb_ep_write(s, eps, offset, value & 0xff, 1);
                inventra_usb_ep_write(s, eps, offset + 1, (value >> 8) & 0xff, 1);
                return;
        }
    }

    qemu_log_mask(LOG_UNIMP, "%s: unimplemented ep write @ 0x%" HWADDR_PRIx ": 0x%" PRIx64 "\n", __func__, offset, value);
}

static uint64_t inventra_usb_read(void *arg, hwaddr offset, unsigned size)
{
    InventraUsbState *s = INVENTRA_USB(arg);
    uint64_t value;

    if (offset >= EP_BASE && offset < EP_BASE + EP_SIZE) {
        offset -= EP_BASE;
        if (s->index == 0) {
            return inventra_usb_ep0_read(s, offset, size);
        } else if (s->index < NUM_EP) {
            return inventra_usb_ep_read(s, &s->eps[s->index - 1], offset, size);
        } else {
            hw_error("%s: invalid ep\n", __func__);
        }
    } else if (offset >= FIFO_BASE && offset < FIFO_BASE + (NUM_EP << 2)) {
        offset = (offset - FIFO_BASE) >> 2;
        fifo_read(offset ? &s->eps[offset - 1].rxfifo : &s->fifo0, (void *) &value, size);
        return value;
    } else if (size == 1) {
        switch (offset) {
            case INTRUSB:
                value = s->intrusb;
                s->intrusb = 0;
                inventra_usb_update_irq(s);
                return value;

            case INTRUSBE:
                return s->intrusbe;

            case DMA_INTR:
                value = s->dma_intr;
                s->dma_intr = 0;
                inventra_usb_update_irq(s);
                return value;
        }
    } else if (size == 2) {
        switch (offset) {
            case INTRTX:
                value = s->intrtx;
                s->intrtx = 0;
                inventra_usb_update_irq(s);
                return value;

            case INTRRX:
                value = s->intrrx;
                s->intrrx = 0;
                inventra_usb_update_irq(s);
                return value;

            case INTRTXE:
                return s->intrtxe;

            case INTRRXE:
                return s->intrrxe;

            case DMA_CNTL:
                return s->dma_cntl;
        }
    } else if (size == 4) {
        switch (offset) {
            case DMA_ADDR:
                return s->dma_addr;

            case DMA_COUNT:
                return s->dma_count;
        }
    }

    qemu_log_mask(LOG_UNIMP, "%s: unimplemented read @ 0x%" HWADDR_PRIx "\n", __func__, offset);
    return 0;
}

static void inventra_usb_write(void *arg, hwaddr offset, uint64_t value, unsigned size)
{
    InventraUsbState *s = INVENTRA_USB(arg);

    if (offset >= EP_BASE && offset < EP_BASE + EP_SIZE) {
        offset -= EP_BASE;
        if (s->index == 0) {
            return inventra_usb_ep0_write(s, offset, value, size);
        } else if (s->index < NUM_EP) {
            return inventra_usb_ep_write(s, &s->eps[s->index - 1], offset, value, size);
        } else {
            hw_error("%s: invalid ep\n", __func__);
        }
    } else if (offset >= FIFO_BASE && offset < FIFO_BASE + (NUM_EP << 2)) {
        offset = (offset - FIFO_BASE) >> 2;
        return fifo_write(offset ? &s->eps[offset - 1].txfifo : &s->fifo0, (void *) &value, size);
    } else if (size == 1) {
        switch (offset) {
            case INTRUSBE:
                if (value & INTRUSB_RESET) {
                    if (tcp_usb_serve(&s->tcp_usb_state, s->port) < 0) {
                        hw_error("%s: failed to start tcp_usb server\n", __func__);
                    }
                }
                s->intrusbe = value;
                inventra_usb_update_irq(s);
                return;

            case INDEX:
                s->index = value;
                return;
        }
    } else if (size == 2) {
        switch (offset) {
            case INTRTXE:
                s->intrtxe = value;
                inventra_usb_update_irq(s);
                return;

            case INTRRXE:
                s->intrrxe = value;
                inventra_usb_update_irq(s);
                return;

            case DMA_CNTL:
                s->dma_cntl = value;
                inventra_usb_update_irq(s);
                return;
        }
    } else if (size == 4) {
        switch (offset) {
            case DMA_ADDR:
                s->dma_addr = value;
                return;

            case DMA_COUNT:
                s->dma_count = value;
                return;
        }
    }

    qemu_log_mask(LOG_UNIMP, "%s: unimplemented write @ 0x%" HWADDR_PRIx ": 0x%" PRIx64 "\n", __func__, offset, value);
}

static void inventra_usb_reset_ep0(InventraUsbState *s)
{
    s->csr0 = 0;
    fifo_flush(&s->fifo0);
}

static void inventra_usb_reset_ep(InventraUsbEpState *eps)
{
    eps->txmaxp = 0;
    eps->txcsrl = 0;
    eps->txcsrh = 0;
    eps->rxcsrl = 0;
    eps->rxcsrh = 0;
    fifo_flush(&eps->txfifo);
    fifo_flush(&eps->rxfifo);
}

static void inventra_usb_reset(DeviceState *dev)
{
    InventraUsbState *s = INVENTRA_USB(dev);
    int i;

    s->intrusb = 0;
    s->intrusbe = 0;
    s->intrtx = 0;
    s->intrtxe = 0;
    s->intrrx = 0;
    s->intrrxe = 0;
    s->index = 0;

    s->dma_intr = 0;
    s->dma_cntl = 0;
    s->dma_addr = 0;
    s->dma_count = 0;

    inventra_usb_reset_ep0(s);
    for (i = 1; i < NUM_EP; i++) {
        inventra_usb_reset_ep(&s->eps[i - 1]);
    }
}

static const struct MemoryRegionOps inventra_usb_ops = {
    .read = inventra_usb_read,
    .write = inventra_usb_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static void inventra_usb_realize(DeviceState *dev, Error **errp)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    InventraUsbState *s = INVENTRA_USB(dev);

    tcp_usb_init(&s->tcp_usb_state, inventra_usb_tcp_callback, s);

    memory_region_init_io(&s->mmio, OBJECT(dev), &inventra_usb_ops, s, TYPE_INVENTRA_USB, 0x350);
    sysbus_init_mmio(sbd, &s->mmio);

    sysbus_init_irq(sbd, &s->intr0);
    sysbus_init_irq(sbd, &s->intr1);
}

static Property inventra_usb_properties[] = {
    DEFINE_PROP_UINT32("port", InventraUsbState, port, 7642),
    DEFINE_PROP_END_OF_LIST(),
};

static void inventra_usb_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = inventra_usb_realize;
    dc->reset = inventra_usb_reset;
    dc->props = inventra_usb_properties;
}

static const TypeInfo inventra_usb_info = {
    .name          = TYPE_INVENTRA_USB,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(InventraUsbState),
    .class_init    = inventra_usb_class_init,
};

static void inventra_usb_register_type(void)
{
    type_register_static(&inventra_usb_info);
}

type_init(inventra_usb_register_type)
