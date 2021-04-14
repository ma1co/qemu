/* QEMU model of the Sony BIONZ dma controller peripheral (similar to PL080) */

#include "qemu/osdep.h"
#include "hw/hw.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/sysbus.h"
#include "qemu/log.h"

#define MAX_CHANNEL 8

#define TYPE_BIONZ_DMA "bionz_dma"
#define BIONZ_DMA(obj) OBJECT_CHECK(DmaState, (obj), TYPE_BIONZ_DMA)

typedef struct LinkedListItem {
    uint32_t src;
    uint32_t dst;
    uint32_t next_lli;
    uint32_t ctrl;
} LinkedListItem;

typedef struct DmaState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    qemu_irq intr[MAX_CHANNEL + 1];

    uint32_t version;
    uint32_t num_channel;

    uint32_t int_reg;
    LinkedListItem regs[MAX_CHANNEL];
    uint32_t conf_reg[MAX_CHANNEL];
    uint32_t lli_reg[MAX_CHANNEL];
} DmaState;

static void dma_update_irq(DmaState *s)
{
    int i;
    for (i = 0; i < s->num_channel; i++) {
        qemu_set_irq(s->intr[i], s->int_reg & (1 << i));
    }
    qemu_set_irq(s->intr[s->num_channel], s->int_reg);
}

static void dma_transfer_mem2mem(DmaState *s, uint32_t src, uint32_t dst, uint32_t size)
{
    void *buffer;

    buffer = g_malloc(size);
    cpu_physical_memory_read(src, buffer, size);
    cpu_physical_memory_write(dst, buffer, size);
    g_free(buffer);
}

static void dma_transfer_mem2peripheral(DmaState *s, uint32_t src, uint32_t dst, uint32_t size)
{
    unsigned char *buffer;
    size_t i, sz;

    buffer = g_malloc(size);
    cpu_physical_memory_read(src, buffer, size);
    for (i = 0; i < size; i += 4) {
        sz = MIN(size - i, 4);
        cpu_physical_memory_write(dst, buffer + i, sz);
    }
    g_free(buffer);
}

static void dma_transfer_peripheral2mem(DmaState *s, uint32_t src, uint32_t dst, uint32_t size)
{
    unsigned char *buffer;
    size_t i, sz;

    buffer = g_malloc(size);
    for (i = 0; i < size; i += 4) {
        sz = MIN(size - i, 4);
        cpu_physical_memory_read(src, buffer + i, sz);
    }
    cpu_physical_memory_write(dst, buffer, size);
    g_free(buffer);
}

static void dma_run(DmaState *s, unsigned ch)
{
    int enable, flow, srcdev, dstdev, sshift, dshift, sinc, dinc, intr;
    uint32_t ldec_ctrl, size;

    while (1) {
        enable = s->conf_reg[ch] & 1;
        flow = (s->conf_reg[ch] >> 11) & 7;
        srcdev = (s->conf_reg[ch] >> 1) & 0xf;
        dstdev = (s->conf_reg[ch] >> 6) & 0xf;

        if (!enable) {
            return;
        }

        LinkedListItem *lli = &s->regs[ch];

        if (s->conf_reg[ch] & 0x2000000) {
            cpu_physical_memory_read(s->lli_reg[ch] & ~3, lli, sizeof(*lli));
            s->conf_reg[ch] &= ~0x2000000;
        }

        switch (s->version) {
            case 1:
                size = lli->ctrl & 0xfff;
                if (size == 0) {
                    size = 0x1000;
                }
                sshift = (lli->ctrl >> 18) & 7;
                dshift = (lli->ctrl >> 21) & 7;
                sinc = (lli->ctrl >> 26) & 1;
                dinc = (lli->ctrl >> 27) & 1;
                intr = (lli->ctrl >> 31) & 1;
                break;

            case 2:
                size = lli->ctrl & 0x7ffff;
                if (size == 0) {
                    size = 0x80000;
                }
                sshift = (lli->ctrl >> 23) & 7;
                dshift = (lli->ctrl >> 26) & 7;
                sinc = (lli->ctrl >> 29) & 1;
                dinc = (lli->ctrl >> 30) & 1;
                intr = (lli->ctrl >> 31) & 1;
                break;

            default:
                hw_error("%s: unknown version\n", __func__);
        }

        if (sshift != dshift || sinc != 1 || dinc != 1) {
            hw_error("%s: unimplemented parameters\n", __func__);
        }

        size <<= sshift;

        switch (flow) {
            case 0:
                dma_transfer_mem2mem(s, lli->src, lli->dst, size);
                break;

            case 1:
                switch (dstdev) {
                    case 6: // ldec
                        break;

                    default:
                        hw_error("%s: unsupported dma peripheral\n", __func__);
                }

                dma_transfer_mem2peripheral(s, lli->src, lli->dst, size);
                break;

            case 2:
                switch (srcdev) {
                    case 7: // ldec
                        cpu_physical_memory_read(lli->src & ~0x7fff, &ldec_ctrl, sizeof(ldec_ctrl));
                        if (!(ldec_ctrl & 2)) {
                            // not enabled
                            return;
                        }
                        break;

                    default:
                        hw_error("%s: unsupported dma peripheral\n", __func__);
                }

                dma_transfer_peripheral2mem(s, lli->src, lli->dst, size);
                break;

            default:
                hw_error("%s: unsupported dma flow\n", __func__);
        }

        switch (s->version) {
            case 1:
                lli->ctrl &= ~0xfff;
                break;

            case 2:
                lli->ctrl &= ~0x7ffff;
                break;

            default:
                hw_error("%s: unknown version\n", __func__);
        }

        if (intr) {
            s->int_reg |= 1 << ch;
            dma_update_irq(s);
        }

        if (lli->next_lli & ~3) {
            cpu_physical_memory_read(lli->next_lli & ~3, lli, sizeof(*lli));
        } else {
            s->conf_reg[ch] &= ~1;
        }
    }
}

static void dma_run_all(DmaState *s)
{
    unsigned ch;

    for (ch = 0; ch < MAX_CHANNEL; ch++) {
        dma_run(s, ch);
    }
}

static uint64_t dma_ch_read(void *opaque, unsigned ch, hwaddr offset, unsigned size)
{
    DmaState *s = BIONZ_DMA(opaque);

    switch (offset) {
        case 0x00:
            return s->regs[ch].src;

        case 0x04:
            return s->regs[ch].dst;

        case 0x08:
            return s->regs[ch].next_lli;

        case 0x0c:
            return s->regs[ch].ctrl;

        case 0x10:
            return s->conf_reg[ch];

        case 0x14:
            return s->lli_reg[ch];

        default:
            qemu_log_mask(LOG_UNIMP, "%s: unimplemented channel read @ 0x%" HWADDR_PRIx "\n", __func__, offset);
            return 0;
    }
}

static void dma_ch_write(void *opaque, unsigned ch, hwaddr offset, uint64_t value, unsigned size)
{
    DmaState *s = BIONZ_DMA(opaque);

    switch (offset) {
        case 0x00:
            s->regs[ch].src = value;
            break;

        case 0x04:
            s->regs[ch].dst = value;
            break;

        case 0x08:
            s->regs[ch].next_lli = value;
            break;

        case 0x0c:
            s->regs[ch].ctrl = value;
            break;

        case 0x10:
            // channel configuration register
            s->conf_reg[ch] = value;
            if (value & 1) {
                dma_run_all(s);
            }
            break;

        case 0x14:
            // channel linked list item register
            s->lli_reg[ch] = value;
            break;

        default:
            qemu_log_mask(LOG_UNIMP, "%s: unimplemented channel write @ 0x%" HWADDR_PRIx ": 0x%" PRIx64 "\n", __func__, offset, value);
    }
}

static uint64_t dma_read(void *opaque, hwaddr offset, unsigned size)
{
    DmaState *s = BIONZ_DMA(opaque);

    if (offset >= 0x100 && offset < 0x100 + (s->num_channel << 5)) {
        return dma_ch_read(s, (offset - 0x100) >> 5, offset & 0x1f, size);
    } else {
        switch (offset) {
            case 0x00:
            case 0x04:
                // interrupt status register
                return s->int_reg;

            case 0x0c:
                // error status register
                return 0;

            default:
                qemu_log_mask(LOG_UNIMP, "%s: unimplemented read @ 0x%" HWADDR_PRIx "\n", __func__, offset);
                return 0;
        }
    }
}

static void dma_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    DmaState *s = BIONZ_DMA(opaque);

    if (offset >= 0x100 && offset < 0x100 + (s->num_channel << 5)) {
        return dma_ch_write(s, (offset - 0x100) >> 5, offset & 0x1f, value, size);
    } else {
        switch (offset) {
            case 0x08:
                // interrupt clear register
                s->int_reg &= ~value;
                dma_update_irq(s);
                break;

            case 0x10:
                // error clear register
                break;

            default:
                qemu_log_mask(LOG_UNIMP, "%s: unimplemented write @ 0x%" HWADDR_PRIx ": 0x%" PRIx64 "\n", __func__, offset, value);
        }
    }
}

static const struct MemoryRegionOps dma_ops = {
    .read = dma_read,
    .write = dma_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void dma_reset(DeviceState *dev)
{
    int i;
    DmaState *s = BIONZ_DMA(dev);

    s->int_reg = 0;
    for (i = 0; i < s->num_channel; i++) {
        memset(&s->regs[i], 0, sizeof(s->regs[i]));
        s->conf_reg[i] = 0;
        s->lli_reg[i] = 0;
    }
}

static void dma_realize(DeviceState *dev, Error **errp)
{
    int i;
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    DmaState *s = BIONZ_DMA(dev);

    memory_region_init_io(&s->mmio, OBJECT(dev), &dma_ops, s, TYPE_BIONZ_DMA, 0x1000);
    sysbus_init_mmio(sbd, &s->mmio);

    for (i = 0; i < s->num_channel + 1; i++) {
        sysbus_init_irq(sbd, &s->intr[i]);
    }
}

static Property dma_properties[] = {
    DEFINE_PROP_UINT32("version", DmaState, version, 0),
    DEFINE_PROP_UINT32("num-channel", DmaState, num_channel, 0),
    DEFINE_PROP_END_OF_LIST(),
};

static void dma_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = dma_realize;
    device_class_set_props(dc, dma_properties);
    dc->reset = dma_reset;
}

static const TypeInfo dma_info = {
    .name          = TYPE_BIONZ_DMA,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(DmaState),
    .class_init    = dma_class_init,
};

static void dma_register_type(void)
{
    type_register_static(&dma_info);
}

type_init(dma_register_type)
