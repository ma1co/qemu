/* QEMU model of the Sony BIONZ boot console */

#include "qemu/osdep.h"
#include "chardev/char-fe.h"
#include "hw/qdev-properties.h"
#include "hw/sysbus.h"
#include "qemu/log.h"

#define TYPE_BIONZ_BOOTCON "bionz_bootcon"
#define BIONZ_BOOTCON(obj) OBJECT_CHECK(BootconState, (obj), TYPE_BIONZ_BOOTCON)

typedef struct BootconState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    CharBackend chr;
} BootconState;

static uint64_t bootcon_read(void *opaque, hwaddr offset, unsigned size)
{
    qemu_log_mask(LOG_UNIMP, "%s: unimplemented read\n", __func__);
    return 0;
}

static void bootcon_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    BootconState *s = BIONZ_BOOTCON(opaque);
    if (value != 0) {
        qemu_chr_fe_write_all(&s->chr, (void *) &value, 1);
    }
}

static const struct MemoryRegionOps bootcon_ops = {
    .read = bootcon_read,
    .write = bootcon_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void bootcon_realize(DeviceState *dev, Error **errp)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    BootconState *s = BIONZ_BOOTCON(dev);
    memory_region_init_io(&s->mmio, OBJECT(dev), &bootcon_ops, s, TYPE_BIONZ_BOOTCON, 4);
    sysbus_init_mmio(sbd, &s->mmio);
}

static Property bootcon_properties[] = {
    DEFINE_PROP_CHR("chardev", BootconState, chr),
    DEFINE_PROP_END_OF_LIST(),
};

static void bootcon_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = bootcon_realize;
    dc->props = bootcon_properties;
}

static const TypeInfo bootcon_info = {
    .name          = TYPE_BIONZ_BOOTCON,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(BootconState),
    .class_init    = bootcon_class_init,
};

static void bootcon_register_type(void)
{
    type_register_static(&bootcon_info);
}

type_init(bootcon_register_type)
