/* QEMU model of the Sony GPIO interrupt controller */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/sysbus.h"
#include "sysemu/sysemu.h"

#define GPIO_DREAD   0x04
#define GPIO_INTLS   0x10
#define GPIO_INTHE   0x14
#define GPIO_INTLE   0x18
#define GPIO_INTEN   0x1c
#define GPIO_INTST   0x20
#define GPIO_INTCL   0x24
#define GPIO_INPUTEN 0x28

#define TYPE_BIONZ_GPIOSYS "bionz_gpiosys"
#define BIONZ_GPIOSYS(obj) OBJECT_CHECK(GpiosysState, (obj), TYPE_BIONZ_GPIOSYS)

typedef struct GpiosysState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    qemu_irq irqs[16];

    uint16_t reg_intls;
    uint16_t reg_inthe;
    uint16_t reg_intle;
    uint16_t reg_inten;
    uint16_t reg_inputen;
    uint16_t gpios;
    uint16_t intst;
} GpiosysState;

static uint16_t gpiosys_get_status(GpiosysState *s)
{
    return s->intst | (s->reg_inputen & s->reg_intls & s->gpios);
}

static void gpiosys_update(GpiosysState *s)
{
    int i;
    for (i = 0; i < 16; i++) {
        qemu_set_irq(s->irqs[i], s->reg_inten & gpiosys_get_status(s) & (1 << i));
    }
}

static void gpiosys_irq_handler(void *opaque, int irq, int level)
{
    GpiosysState *s = BIONZ_GPIOSYS(opaque);

    if (level) {
        if (s->reg_inputen & s->reg_inthe & ~s->gpios & (1 << irq)) {
            s->intst |= (1 << irq);
        }
        s->gpios |= (1 << irq);
    } else {
        if (s->reg_inputen & s->reg_intle & s->gpios & (1 << irq)) {
            s->intst |= (1 << irq);
        }
        s->gpios &= ~(1 << irq);
    }

    gpiosys_update(s);
}

static uint64_t gpiosys_read(void *opaque, hwaddr offset, unsigned size)
{
    GpiosysState *s = BIONZ_GPIOSYS(opaque);

    switch (offset) {
        case GPIO_DREAD:
            return s->gpios;

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

        case GPIO_INPUTEN:
            return s->reg_inputen;

        default:
            qemu_log_mask(LOG_UNIMP, "%s: unimplemented read @ 0x%" HWADDR_PRIx "\n", __func__, offset);
            return 0;
    }
}

static void gpiosys_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    GpiosysState *s = BIONZ_GPIOSYS(opaque);

    switch (offset) {
        case GPIO_INTLS:
            s->reg_intls = value;
            gpiosys_update(s);
            break;

        case GPIO_INTHE:
            s->reg_inthe |= value;
            break;

        case GPIO_INTLE:
            s->reg_intle |= value;
            break;

        case GPIO_INTEN:
            s->reg_inten = value;
            gpiosys_update(s);
            break;

        case GPIO_INTCL:
            s->intst &= ~value;
            gpiosys_update(s);
            break;

        case GPIO_INPUTEN:
            s->reg_inputen = value;
            gpiosys_update(s);
            break;

        default:
            qemu_log_mask(LOG_UNIMP, "%s: unimplemented write @ 0x%" HWADDR_PRIx ": 0x%" PRIx64 "\n", __func__, offset, value);
    }
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

    s->reg_intls = 0;
    s->reg_inthe = 0;
    s->reg_intle = 0;
    s->reg_inten = 0;
    s->reg_inputen = 0;
    s->gpios = 0;
    s->intst = 0;
}

static int gpiosys_init(SysBusDevice *sbd)
{
    int i;
    GpiosysState *s = BIONZ_GPIOSYS(sbd);

    memory_region_init_io(&s->mmio, OBJECT(sbd), &gpiosys_ops, s, TYPE_BIONZ_GPIOSYS, 0x100);
    sysbus_init_mmio(sbd, &s->mmio);

    qdev_init_gpio_in(DEVICE(sbd), gpiosys_irq_handler, 16);
    for (i = 0; i < 16; i++) {
        sysbus_init_irq(sbd, &s->irqs[i]);
    }

    return 0;
}

static void gpiosys_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SysBusDeviceClass *k = SYS_BUS_DEVICE_CLASS(klass);

    k->init = gpiosys_init;
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
