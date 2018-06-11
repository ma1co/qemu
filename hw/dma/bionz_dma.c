/* QEMU model of the Sony BIONZ dma controller peripheral (similar to PL080) */

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "qemu/log.h"

#define NUM_CHANNEL 4

#define TYPE_BIONZ_DMA "bionz_dma"
#define BIONZ_DMA(obj) OBJECT_CHECK(DmaState, (obj), TYPE_BIONZ_DMA)

typedef struct DmaState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    AddressSpace as;
    qemu_irq intr[NUM_CHANNEL];

    uint32_t int_reg;
    uint32_t lli_reg[NUM_CHANNEL];
} DmaState;

typedef struct LinkedListItem {
    uint32_t src;
    uint32_t dst;
    uint32_t next_lli;
    uint32_t ctrl;
} LinkedListItem;

static void dma_update_irq(DmaState *s)
{
    int i;
    for (i = 0; i < NUM_CHANNEL; i++) {
        qemu_set_irq(s->intr[i], s->int_reg & (1 << i));
    }
}

static void dma_run(DmaState *s, unsigned ch)
{
    uint32_t lli_ptr = s->lli_reg[ch] & ~3;
    LinkedListItem lli;
    int size, sshift, dshift, sinc, dinc, intr;

    while (lli_ptr) {
        if (address_space_read(&s->as, lli_ptr, MEMTXATTRS_UNSPECIFIED, (void *) &lli, sizeof(lli)) != MEMTX_OK) {
            hw_error("%s: cannot read from 0x%" PRIx32 "\n", __func__, lli_ptr);
        }

        size = lli.ctrl & 0x7ffff;
        if (size == 0) {
            size = 0x80000;
        }
        sshift = (lli.ctrl >> 23) & 7;
        dshift = (lli.ctrl >> 26) & 7;
        sinc = (lli.ctrl >> 29) & 1;
        dinc = (lli.ctrl >> 30) & 1;
        intr = (lli.ctrl >> 31) & 1;

        if (sshift != dshift || sinc != 1 || dinc != 1) {
            hw_error("%s: unimplemented parameters\n", __func__);
        }

        void *buffer = g_malloc(size << sshift);
        if (address_space_read(&s->as, lli.src, MEMTXATTRS_UNSPECIFIED, buffer, size << sshift) != MEMTX_OK) {
            hw_error("%s: cannot read from 0x%" PRIx32 "\n", __func__, lli.src);
        }
        if (address_space_write(&s->as, lli.dst, MEMTXATTRS_UNSPECIFIED, buffer, size << dshift) != MEMTX_OK) {
            hw_error("%s: cannot write to 0x%" PRIx32 "\n", __func__, lli.dst);
        }
        g_free(buffer);

        if (intr) {
            s->int_reg |= 1 << ch;
            dma_update_irq(s);
        }

        lli_ptr = lli.next_lli;
    }
}

static uint64_t dma_ch_read(void *opaque, unsigned ch, hwaddr offset, unsigned size)
{
    qemu_log_mask(LOG_UNIMP, "%s: unimplemented channel read @ 0x%" HWADDR_PRIx "\n", __func__, offset);
    return 0;
}

static void dma_ch_write(void *opaque, unsigned ch, hwaddr offset, uint64_t value, unsigned size)
{
    DmaState *s = BIONZ_DMA(opaque);

    switch (offset) {
        case 0x10:
            // channel configuration register
            if (value & 1) {
                dma_run(s, ch);
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

    if (offset >= 0x100 && offset < 0x100 + (NUM_CHANNEL << 5)) {
        return dma_ch_read(s, (offset - 0x100) >> 5, offset & 0x1f, size);
    } else {
        switch (offset) {
            case 0x00:
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

    if (offset >= 0x100 && offset < 0x100 + (NUM_CHANNEL << 5)) {
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
    for (i = 0; i < NUM_CHANNEL; i++) {
        s->lli_reg[i] = 0;
    }
}

static int dma_init(SysBusDevice *sbd)
{
    int i;
    DmaState *s = BIONZ_DMA(sbd);

    address_space_init(&s->as, sysbus_address_space(sbd), "as");

    memory_region_init_io(&s->mmio, OBJECT(sbd), &dma_ops, s, TYPE_BIONZ_DMA, 0x1000);
    sysbus_init_mmio(sbd, &s->mmio);

    for (i = 0; i < NUM_CHANNEL; i++) {
        sysbus_init_irq(sbd, &s->intr[i]);
    }

    return 0;
}

static void dma_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SysBusDeviceClass *k = SYS_BUS_DEVICE_CLASS(klass);

    k->init = dma_init;
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
