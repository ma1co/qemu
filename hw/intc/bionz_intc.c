/* QEMU model of the Sony interrupt controller */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "hw/irq.h"
#include "hw/sysbus.h"
#include "sysemu/sysemu.h"

#define IRQ_STATUS       0x00
#define FIQ_STATUS       0x04
#define IRQ_RAW_STATUS   0x08
#define IRQ_SELECT       0x0c
#define IRQ_ENABLE       0x10
#define IRQ_ENABLE_SET   0x10
#define IRQ_ENABLE_CLEAR 0x14

#define CH_RAW_STATUS    0x00
#define CH_STATUS        0x04
#define CH_ENABLE        0x08
#define CH_ENABLE_SET    0x08
#define CH_ENABLE_CLEAR  0x0c

#define TYPE_BIONZ_INTC "bionz_intc"
#define BIONZ_INTC(obj) OBJECT_CHECK(IntcState, (obj), TYPE_BIONZ_INTC)

typedef struct IntcState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    qemu_irq irq;
    qemu_irq fiq;

    uint32_t reg_status;
    uint32_t reg_select;
    uint32_t reg_enable;

    uint16_t ch_status[32];
    uint16_t ch_enable[32];
} IntcState;

static void intc_update(IntcState *s)
{
    int i;

    s->reg_status = 0;
    for (i = 0; i < 32; i++) {
        if (s->ch_status[i] & s->ch_enable[i]) {
            s->reg_status |= (1 << i);
        }
    }

    qemu_set_irq(s->irq, s->reg_status & s->reg_enable & ~s->reg_select);
    qemu_set_irq(s->fiq, s->reg_status & s->reg_enable & s->reg_select);
}

static void intc_irq_handler(void *opaque, int irq, int level)
{
    unsigned ch;
    IntcState *s = BIONZ_INTC(opaque);

    ch = irq >> 4;
    irq &= 0xf;

    if (level) {
        s->ch_status[ch] |= (1 << irq);
    } else {
        s->ch_status[ch] &= ~(1 << irq);
    }

    intc_update(s);
}

static uint64_t intc_ch_read(IntcState *s, unsigned ch, hwaddr offset, unsigned size)
{
    switch (offset) {
        case CH_RAW_STATUS:
            return s->ch_status[ch];

        case CH_STATUS:
            return s->ch_status[ch] & s->ch_enable[ch];

        case CH_ENABLE:
            return s->ch_enable[ch];

        default:
            qemu_log_mask(LOG_UNIMP, "%s: unimplemented channel read @ 0x%" HWADDR_PRIx "\n", __func__, offset);
            return 0;
    }
}

static void intc_ch_write(IntcState *s, unsigned ch, hwaddr offset, uint64_t value, unsigned size)
{
    switch (offset) {
        case CH_ENABLE_SET:
            s->ch_enable[ch] |= value;
            intc_update(s);
            break;

        case CH_ENABLE_CLEAR:
            s->ch_enable[ch] &= ~value;
            intc_update(s);
            break;

        default:
            qemu_log_mask(LOG_UNIMP, "%s: unimplemented channel write @ 0x%" HWADDR_PRIx ": 0x%" PRIx64 "\n", __func__, offset, value);
    }
}

static uint64_t intc_read(void *opaque, hwaddr offset, unsigned size)
{
    IntcState *s = BIONZ_INTC(opaque);

    if (offset >= 0x100 && offset <= 0x500) {
        offset -= 0x100;
        return intc_ch_read(s, offset >> 5, offset & 0x1f, size);
    } else {
        switch (offset) {
            case IRQ_STATUS:
                return s->reg_status & s->reg_enable & ~s->reg_select;

            case FIQ_STATUS:
                return s->reg_status & s->reg_enable & s->reg_select;

            case IRQ_RAW_STATUS:
                return s->reg_status;

            case IRQ_SELECT:
                return s->reg_select;

            case IRQ_ENABLE:
                return s->reg_enable;

            default:
                qemu_log_mask(LOG_UNIMP, "%s: unimplemented read @ 0x%" HWADDR_PRIx "\n", __func__, offset);
                return 0;
        }
    }
}

static void intc_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    IntcState *s = BIONZ_INTC(opaque);

    if (offset >= 0x100 && offset <= 0x500) {
        offset -= 0x100;
        intc_ch_write(s, offset >> 5, offset & 0x1f, value, size);
    } else {
        switch (offset) {
            case IRQ_SELECT:
                s->reg_select = value;
                intc_update(s);
                break;

            case IRQ_ENABLE_SET:
                s->reg_enable |= value;
                intc_update(s);
                break;

            case IRQ_ENABLE_CLEAR:
                s->reg_enable &= ~value;
                intc_update(s);
                break;

            default:
                qemu_log_mask(LOG_UNIMP, "%s: unimplemented write @ 0x%" HWADDR_PRIx ": 0x%" PRIx64 "\n", __func__, offset, value);
        }
    }
}

static const struct MemoryRegionOps intc_ops = {
    .read = intc_read,
    .write = intc_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void intc_reset(DeviceState *dev)
{
    int i;
    IntcState *s = BIONZ_INTC(dev);

    s->reg_status = 0;
    s->reg_select = 0;
    s->reg_enable = 0xffffffff;

    for (i = 0; i < 32; i++) {
        s->ch_status[i] = 0;
        s->ch_enable[i] = 0xffff;
    }
}

static void intc_realize(DeviceState *dev, Error **errp)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    IntcState *s = BIONZ_INTC(dev);

    memory_region_init_io(&s->mmio, OBJECT(dev), &intc_ops, s, TYPE_BIONZ_INTC, 0x500);
    sysbus_init_mmio(sbd, &s->mmio);

    qdev_init_gpio_in(dev, intc_irq_handler, 32 * 16);
    sysbus_init_irq(sbd, &s->irq);
    sysbus_init_irq(sbd, &s->fiq);
}

static void intc_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = intc_realize;
    dc->reset = intc_reset;
}

static const TypeInfo intc_info = {
    .name          = TYPE_BIONZ_INTC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IntcState),
    .class_init    = intc_class_init,
};

static void intc_register_type(void)
{
    type_register_static(&intc_info);
}

type_init(intc_register_type)
