/* QEMU model of the Sony GPIO registers */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/hw.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/sysbus.h"

#define GPIO_V1_DIR     0x00
#define GPIO_V1_RDATA   0x04
#define GPIO_V1_DATASET 0x08
#define GPIO_V1_DATACLR 0x0c
#define GPIO_V1_INEN    0x10

#define GPIO_V2_DIR     0x00
#define GPIO_V2_DIRSET  0x04
#define GPIO_V2_DIRCLR  0x08
#define GPIO_V2_RDATA   0x10
#define GPIO_V2_DATASET 0x14
#define GPIO_V2_DATACLR 0x18
#define GPIO_V2_INEN    0x20
#define GPIO_V2_INENSET 0x24
#define GPIO_V2_INENCLR 0x28

#define GPIO_V3_DIR     0x00
#define GPIO_V3_DIRSET  0x04
#define GPIO_V3_DIRCLR  0x08
#define GPIO_V3_RDATA   0x10
#define GPIO_V3_INEN    0x20
#define GPIO_V3_INENSET 0x24
#define GPIO_V3_INENCLR 0x28
#define GPIO_V3_WDATA   0x40
#define GPIO_V3_DATASET 0x44
#define GPIO_V3_DATACLR 0x48

#define TYPE_BIONZ_GPIO "bionz_gpio"
#define BIONZ_GPIO(obj) OBJECT_CHECK(GpioState, (obj), TYPE_BIONZ_GPIO)

typedef struct GpioState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    qemu_irq irqs[32];
    qemu_irq outputs[32];

    uint8_t version;
    uint8_t num_gpio;

    uint32_t reg_dir;
    uint32_t reg_wdata;
    uint32_t reg_inen;
    uint32_t rdata;
} GpioState;

static uint32_t gpio_get_rdata(GpioState *s)
{
    return (~s->reg_dir & s->reg_inen & s->rdata) | (s->reg_dir & s->reg_wdata);
}

static void gpio_update(GpioState *s)
{
    int i;
    for (i = 0; i < s->num_gpio; i++) {
        qemu_set_irq(s->outputs[i], ((s->reg_dir & s->reg_wdata) >> i) & 1);
        qemu_set_irq(s->irqs[i], (gpio_get_rdata(s) >> i) & 1);
    }
}

static void gpio_input_handler(void *opaque, int irq, int level)
{
    GpioState *s = BIONZ_GPIO(opaque);

    if (level) {
        s->rdata |= (1 << irq);
    } else {
        s->rdata &= ~(1 << irq);
    }

    gpio_update(s);
}

static uint64_t gpio_read(void *opaque, hwaddr offset, unsigned size)
{
    GpioState *s = BIONZ_GPIO(opaque);

    if (s->num_gpio > 16 && size != 4) {
        goto error;
    }

    switch (s->version) {
    case 1:
        switch (offset) {
        case GPIO_V1_DIR:
            return s->reg_dir;

        case GPIO_V1_RDATA:
            return gpio_get_rdata(s);

        case GPIO_V1_INEN:
            return s->reg_inen;
        }
        break;

    case 2:
        switch (offset) {
        case GPIO_V2_DIR:
            return s->reg_dir;

        case GPIO_V2_RDATA:
            return gpio_get_rdata(s);

        case GPIO_V2_INEN:
            return s->reg_inen;
        }
        break;

    case 3:
        switch (offset) {
        case GPIO_V3_DIR:
            return s->reg_dir;

        case GPIO_V3_RDATA:
            return gpio_get_rdata(s);

        case GPIO_V3_INEN:
            return s->reg_inen;

        case GPIO_V3_WDATA:
            return s->reg_wdata;
        }
        break;

    default:
        hw_error("%s: unknown version\n", __func__);
        return 0;
    }

error:
    qemu_log_mask(LOG_UNIMP, "%s: unimplemented read @ 0x%" HWADDR_PRIx "\n", __func__, offset);
    return 0;
}

static void gpio_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    GpioState *s = BIONZ_GPIO(opaque);

    if (s->num_gpio > 16 && size != 4) {
        goto error;
    }

    switch (s->version) {
    case 1:
        switch (offset) {
        case GPIO_V1_DIR:
            s->reg_dir = value;
            break;

        case GPIO_V1_DATASET:
            s->reg_wdata |= value;
            break;

        case GPIO_V1_DATACLR:
           s->reg_wdata &= ~value;
           break;

        case GPIO_V1_INEN:
            s->reg_inen = value;
            break;

        default:
            goto error;
        }
        break;

    case 2:
        switch (offset) {
        case GPIO_V2_DIRSET:
            s->reg_dir |= value;
            break;

        case GPIO_V2_DIRCLR:
            s->reg_dir &= ~value;
            break;

        case GPIO_V2_DATASET:
            s->reg_wdata |= value;
            break;

        case GPIO_V2_DATACLR:
           s->reg_wdata &= ~value;
           break;

        case GPIO_V2_INENSET:
            s->reg_inen |= value;
            break;

        case GPIO_V2_INENCLR:
            s->reg_inen &= ~value;
            break;

        default:
            goto error;
        }
        break;

    case 3:
        switch (offset) {
        case GPIO_V3_DIRSET:
            s->reg_dir |= value;
            break;

        case GPIO_V3_DIRCLR:
            s->reg_dir &= ~value;
            break;

        case GPIO_V3_INENSET:
            s->reg_inen |= value;
            break;

        case GPIO_V3_INENCLR:
            s->reg_inen &= ~value;
            break;

        case GPIO_V3_DATASET:
            s->reg_wdata |= value;
            break;

        case GPIO_V3_DATACLR:
           s->reg_wdata &= ~value;
           break;

        default:
            goto error;
        }
        break;

    default:
        hw_error("%s: unknown version\n", __func__);
        return;
    }

    gpio_update(s);
    return;

error:
    qemu_log_mask(LOG_UNIMP, "%s: unimplemented write @ 0x%" HWADDR_PRIx ": 0x%" PRIx64 "\n", __func__, offset, value);
}

static const struct MemoryRegionOps gpio_ops = {
    .read = gpio_read,
    .write = gpio_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 2,
    .valid.max_access_size = 4,
};

static void gpio_reset(DeviceState *dev)
{
    GpioState *s = BIONZ_GPIO(dev);

    s->reg_dir = 0;
    s->reg_wdata = 0;
    s->reg_inen = 0;
    s->rdata = 0;

    gpio_update(s);
}

static void gpio_realize(DeviceState *dev, Error **errp)
{
    int i;
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    GpioState *s = BIONZ_GPIO(dev);

    memory_region_init_io(&s->mmio, OBJECT(dev), &gpio_ops, s, TYPE_BIONZ_GPIO, 0x100);
    sysbus_init_mmio(sbd, &s->mmio);

    assert(s->num_gpio <= 32);
    qdev_init_gpio_in(dev, gpio_input_handler, s->num_gpio);
    qdev_init_gpio_out(dev, s->outputs, s->num_gpio);
    for (i = 0; i < s->num_gpio; i++) {
        sysbus_init_irq(sbd, &s->irqs[i]);
    }
}

static Property gpio_properties[] = {
    DEFINE_PROP_UINT8("version", GpioState, version, 0),
    DEFINE_PROP_UINT8("num-gpio", GpioState, num_gpio, 32),
    DEFINE_PROP_END_OF_LIST(),
};

static void gpio_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = gpio_realize;
    dc->reset = gpio_reset;
    device_class_set_props(dc, gpio_properties);
}

static const TypeInfo gpio_info = {
    .name          = TYPE_BIONZ_GPIO,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(GpioState),
    .class_init    = gpio_class_init,
};

static void gpio_register_type(void)
{
    type_register_static(&gpio_info);
}

type_init(gpio_register_type)
