/* QEMU model of the ARM PrimeCell inter-processor communications module (PL320) */

#include "qemu/osdep.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/sysbus.h"
#include "qemu/log.h"

#define MAX_MBOX 32
#define MAX_INTR 32
#define MAX_DATA 7

#define TYPE_PL320 "pl320"
#define PL320(obj) OBJECT_CHECK(Pl320State, (obj), TYPE_PL320)

typedef struct Pl320Mbox {
    uint32_t src;
    uint32_t dst;
    uint32_t mask;
    uint32_t send;
    uint32_t data[MAX_DATA];
} Pl320Mbox;

typedef struct Pl320Intr {
    uint32_t masked;
    uint32_t raw;
} Pl320Intr;

typedef struct Pl320State {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    qemu_irq irqs[MAX_INTR];

    uint32_t mboxnum;
    uint32_t intrnum;
    uint32_t datanum;

    Pl320Mbox mbox[MAX_MBOX];
    Pl320Intr intr[MAX_INTR];
} Pl320State;

static const uint8_t pl320_ids[] = {
    0x20, 0x13, 0x04, 0x00,
    0x0d, 0xf0, 0x05, 0xb1,
};

static void pl320_update_irq(Pl320State *s)
{
    int i, j;
    Pl320Intr *intr;
    Pl320Mbox *mbox;

    for (i = 0; i < s->intrnum; i++) {
        intr = &s->intr[i];
        intr->masked = 0;
        intr->raw = 0;
        for (j = 0; j < s->mboxnum; j++) {
            mbox = &s->mbox[j];
            if ((mbox->send & 2 && mbox->src & (1 << i)) || (mbox->send & 1 && mbox->dst & (1 << i))) {
                intr->raw |= (1 << j);
                if (mbox->mask & (1 << i)) {
                    intr->masked |= (1 << j);
                }
            }
        }
        qemu_set_irq(s->irqs[i], intr->masked);
    }
}

static uint64_t pl320_intr_read(Pl320State *s, unsigned idx, hwaddr offset, unsigned size)
{
    Pl320Intr *intr = &s->intr[idx];

    switch (offset) {
        case 0x00:
            return intr->masked;

        case 0x04:
            return intr->raw;

        default:
            qemu_log_mask(LOG_UNIMP, "%s: unimplemented intr read @ 0x%" HWADDR_PRIx "\n", __func__, offset);
            return 0;
    }
}

static void pl320_mbox_reset(Pl320State *s, unsigned idx)
{
    int i;
    Pl320Mbox *mbox = &s->mbox[idx];

    mbox->src = 0;
    mbox->dst = 0;
    mbox->mask = 0;
    mbox->send = 0;

    for (i = 0; i < MAX_DATA; i++) {
        mbox->data[i] = 0;
    }
}

static uint64_t pl320_mbox_read(Pl320State *s, unsigned idx, hwaddr offset, unsigned size)
{
    Pl320Mbox *mbox = &s->mbox[idx];

    switch (offset) {
        case 0x00:
            return mbox->src;

        case 0x0c:
            return mbox->dst;

        case 0x18:
            return mbox->mask;

        case 0x20:
            return mbox->send;

        case 0x24 ... 0x3f:
            return mbox->data[(offset - 0x24) >> 2];

        default:
            qemu_log_mask(LOG_UNIMP, "%s: unimplemented mbox read @ 0x%" HWADDR_PRIx "\n", __func__, offset);
            return 0;
    }
}

static void pl320_mbox_write(Pl320State *s, unsigned idx, hwaddr offset, uint64_t value, unsigned size)
{
    Pl320Mbox *mbox = &s->mbox[idx];

    switch (offset) {
        case 0x00:
            if (value == 0) {
                pl320_mbox_reset(s, idx);
            } else if (mbox->src == 0) {
                mbox->src = value;
            }
            pl320_update_irq(s);
            break;

        case 0x04:
            mbox->dst |= value;
            pl320_update_irq(s);
            break;

        case 0x08:
            mbox->dst &= ~value;
            pl320_update_irq(s);
            break;

        case 0x14:
            mbox->mask |= value;
            pl320_update_irq(s);
            break;

        case 0x18:
            mbox->mask &= ~value;
            pl320_update_irq(s);
            break;

        case 0x20:
            mbox->send = value;
            pl320_update_irq(s);
            break;

        case 0x24 ... 0x3f:
            mbox->data[(offset - 0x24) >> 2] = value;
            break;

        default:
            qemu_log_mask(LOG_UNIMP, "%s: unimplemented mbox write @ 0x%" HWADDR_PRIx ": 0x%" PRIx64 "\n", __func__, offset, value);
    }
}

static uint64_t pl320_read(void *opaque, hwaddr offset, unsigned size)
{
    Pl320State *s = PL320(opaque);

    if (offset < s->mboxnum * 0x40) {
        return pl320_mbox_read(s, offset >> 6, offset & 0x3f, size);
    } else if (offset >= 0x800 && offset < 0x800 + s->mboxnum * 8) {
        return pl320_intr_read(s, (offset - 0x800) >> 3, offset & 7, size);
    } else {
        switch (offset) {
            case 0x900:
                return (s->mboxnum << 16) + (s->intrnum << 8) + s->datanum;

            case 0xfe0 ... 0xfff:
                return pl320_ids[(offset - 0xfe0) >> 2];

            default:
                qemu_log_mask(LOG_UNIMP, "%s: unimplemented read @ 0x%" HWADDR_PRIx "\n", __func__, offset);
                return 0;
        }
    }
}

static void pl320_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    Pl320State *s = PL320(opaque);

    if (offset < s->mboxnum * 0x40) {
        pl320_mbox_write(s, offset >> 6, offset & 0x3f, value, size);
    } else {
        qemu_log_mask(LOG_UNIMP, "%s: unimplemented write @ 0x%" HWADDR_PRIx ": 0x%" PRIx64 "\n", __func__, offset, value);
    }
}

static const struct MemoryRegionOps pl320_ops = {
    .read = pl320_read,
    .write = pl320_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void pl320_reset(DeviceState *dev)
{
    int i;
    Pl320State *s = PL320(dev);

    for (i = 0; i < MAX_MBOX; i++) {
        pl320_mbox_reset(s, i);
    }
    pl320_update_irq(s);
}

static void pl320_realize(DeviceState *dev, Error **errp)
{
    int i;
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    Pl320State *s = PL320(dev);

    memory_region_init_io(&s->mmio, OBJECT(dev), &pl320_ops, s, TYPE_PL320, 0x1000);
    sysbus_init_mmio(sbd, &s->mmio);

    for (i = 0; i < MAX_INTR; i++) {
        sysbus_init_irq(sbd, &s->irqs[i]);
    }
}

static Property pl320_properties[] = {
    DEFINE_PROP_UINT32("mboxnum", Pl320State, mboxnum, MAX_MBOX),
    DEFINE_PROP_UINT32("intrnum", Pl320State, intrnum, MAX_INTR),
    DEFINE_PROP_UINT32("datanum", Pl320State, datanum, MAX_DATA),
    DEFINE_PROP_END_OF_LIST(),
};

static void pl320_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = pl320_realize;
    dc->reset = pl320_reset;
    device_class_set_props(dc, pl320_properties);
}

static const TypeInfo pl320_info = {
    .name          = TYPE_PL320,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Pl320State),
    .class_init    = pl320_class_init,
};

static void pl320_register_type(void)
{
    type_register_static(&pl320_info);
}

type_init(pl320_register_type)
