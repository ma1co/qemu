/* QEMU model of the Sony GPIO interrupt controller */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/sysbus.h"

#define GPIO_DIR     0x00
#define GPIO_RDATA   0x04
#define GPIO_DATASET 0x08
#define GPIO_DATACLR 0x0c
#define GPIO_INTLS   0x10
#define GPIO_INTHE   0x14
#define GPIO_INTLE   0x18
#define GPIO_INTEN   0x1c
#define GPIO_INTST   0x20
#define GPIO_INTCL   0x24
#define GPIO_INEN    0x28

#define TYPE_BIONZ_GPIOSYS "bionz_gpiosys"
#define BIONZ_GPIOSYS(obj) OBJECT_CHECK(GpiosysState, (obj), TYPE_BIONZ_GPIOSYS)

typedef struct GpiosysState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    qemu_irq irqs[16];
    qemu_irq outputs[16];

    uint16_t reg_dir;
    uint16_t reg_wdata;
    uint16_t reg_intls;
    uint16_t reg_inthe;
    uint16_t reg_intle;
    uint16_t reg_inten;
    uint16_t reg_inen;
    uint16_t rdata;
    uint16_t intst;
} GpiosysState;

static uint16_t gpiosys_get_status(GpiosysState *s)
{
    return s->intst | (~s->reg_dir & s->reg_inen & s->reg_intls & s->rdata);
}

static void gpiosys_update(GpiosysState *s)
{
    int i;
    for (i = 0; i < 16; i++) {
        qemu_set_irq(s->outputs[i], ((s->reg_dir & s->reg_wdata) >> i) & 1);
        qemu_set_irq(s->irqs[i], ((s->reg_inten & gpiosys_get_status(s)) >> i) & 1);
    }
}

static void gpiosys_input_handler(void *opaque, int irq, int level)
{
    GpiosysState *s = BIONZ_GPIOSYS(opaque);

    if (level) {
        if (~s->reg_dir & s->reg_inen & s->reg_inthe & ~s->rdata & (1 << irq)) {
            s->intst |= (1 << irq);
        }
        s->rdata |= (1 << irq);
    } else {
        if (~s->reg_dir & s->reg_inen & s->reg_intle & s->rdata & (1 << irq)) {
            s->intst |= (1 << irq);
        }
        s->rdata &= ~(1 << irq);
    }

    gpiosys_update(s);
}

static uint64_t gpiosys_read(void *opaque, hwaddr offset, unsigned size)
{
    GpiosysState *s = BIONZ_GPIOSYS(opaque);

    switch (offset) {
        case GPIO_DIR:
            return s->reg_dir;

        case GPIO_RDATA:
            return ~s->reg_dir & s->reg_inen & s->rdata;

        case GPIO_INTLS:
            return s->reg_intls;

        case GPIO_INTHE:
            return s->reg_inthe;

        case GPIO_INTLE:
            return s->reg_intle;

        case GPIO_INTEN:
            return s->reg_inten;

        case GPIO_INTST:
            return gpiosys_get_status(s);

        case GPIO_INEN:
            return s->reg_inen;

        default:
            qemu_log_mask(LOG_UNIMP, "%s: unimplemented read @ 0x%" HWADDR_PRIx "\n", __func__, offset);
            return 0;
    }
}

static void gpiosys_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    GpiosysState *s = BIONZ_GPIOSYS(opaque);

    switch (offset) {
        case GPIO_DIR:
            s->reg_dir = value;
            break;

        case GPIO_DATASET:
            s->reg_wdata |= value;
            break;

        case GPIO_DATACLR:
           s->reg_wdata &= ~value;
           break;

        case GPIO_INTLS:
            s->reg_intls = value;
            break;

        case GPIO_INTHE:
            s->reg_inthe |= value;
            break;

        case GPIO_INTLE:
            s->reg_intle |= value;
            break;

        case GPIO_INTEN:
            s->reg_inten = value;
            break;

        case GPIO_INTCL:
            s->intst &= ~value;
            break;

        case GPIO_INEN:
            s->reg_inen = value;
            break;

        default:
            qemu_log_mask(LOG_UNIMP, "%s: unimplemented write @ 0x%" HWADDR_PRIx ": 0x%" PRIx64 "\n", __func__, offset, value);
    }

    gpiosys_update(s);
}

static const struct MemoryRegionOps gpiosys_ops = {
    .read = gpiosys_read,
    .write = gpiosys_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void gpiosys_reset(DeviceState *dev)
{
    GpiosysState *s = BIONZ_GPIOSYS(dev);

    s->reg_dir = 0;
    s->reg_wdata = 0;
    s->reg_intls = 0;
    s->reg_inthe = 0;
    s->reg_intle = 0;
    s->reg_inten = 0;
    s->reg_inen = 0;
    s->rdata = 0;
    s->intst = 0;

    gpiosys_update(s);
}

static void gpiosys_realize(DeviceState *dev, Error **errp)
{
    int i;
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    GpiosysState *s = BIONZ_GPIOSYS(dev);

    memory_region_init_io(&s->mmio, OBJECT(dev), &gpiosys_ops, s, TYPE_BIONZ_GPIOSYS, 0x100);
    sysbus_init_mmio(sbd, &s->mmio);

    qdev_init_gpio_in(dev, gpiosys_input_handler, 16);
    qdev_init_gpio_out(dev, s->outputs, 16);
    for (i = 0; i < 16; i++) {
        sysbus_init_irq(sbd, &s->irqs[i]);
    }
}

static void gpiosys_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = gpiosys_realize;
    dc->reset = gpiosys_reset;
}

static const TypeInfo gpiosys_info = {
    .name          = TYPE_BIONZ_GPIOSYS,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(GpiosysState),
    .class_init    = gpiosys_class_init,
};

static void gpiosys_register_type(void)
{
    type_register_static(&gpiosys_info);
}

type_init(gpiosys_register_type)
