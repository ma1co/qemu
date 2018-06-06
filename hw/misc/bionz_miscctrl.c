/* QEMU model of the Sony BIONZ miscctrl peripheral */

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "qemu/log.h"

#define MISCCTRL_MODEREAD 0x00
#define MISCCTRL_READDONE 0x10
#define MISCCTRL_TYPEID 0x20

#define TYPE_BIONZ_MISCCTRL "bionz_miscctrl"
#define BIONZ_MISCCTRL(obj) OBJECT_CHECK(MiscctrlState, (obj), TYPE_BIONZ_MISCCTRL)

typedef struct MiscctrlState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;

    uint32_t mode;
    uint32_t typeid;
} MiscctrlState;

static uint64_t miscctrl_read(void *opaque, hwaddr offset, unsigned size)
{
    MiscctrlState *s = BIONZ_MISCCTRL(opaque);

    switch (offset) {
        case MISCCTRL_MODEREAD:
            return s->mode;

        case MISCCTRL_READDONE:
            return 1;

        case MISCCTRL_TYPEID:
            return s->typeid;

        default:
            qemu_log_mask(LOG_UNIMP, "%s: unimplemented read @ 0x%" HWADDR_PRIx "\n", __func__, offset);
            return 0;
    }
}

static void miscctrl_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    qemu_log_mask(LOG_UNIMP, "%s: unimplemented  write @ 0x%" HWADDR_PRIx ": 0x%" PRIx64 "\n", __func__, offset, value);
}

static const struct MemoryRegionOps miscctrl_ops = {
    .read = miscctrl_read,
    .write = miscctrl_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static int miscctrl_init(SysBusDevice *sbd)
{
    MiscctrlState *s = BIONZ_MISCCTRL(sbd);

    memory_region_init_io(&s->mmio, OBJECT(sbd), &miscctrl_ops, s, TYPE_BIONZ_MISCCTRL, 0x100);
    sysbus_init_mmio(sbd, &s->mmio);

    return 0;
}

static Property miscctrl_properties[] = {
    DEFINE_PROP_UINT32("mode", MiscctrlState, mode, 0),
    DEFINE_PROP_UINT32("typeid", MiscctrlState, typeid, 0),
    DEFINE_PROP_END_OF_LIST(),
};

static void miscctrl_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SysBusDeviceClass *k = SYS_BUS_DEVICE_CLASS(klass);

    k->init = miscctrl_init;
    dc->props = miscctrl_properties;
}

static const TypeInfo miscctrl_info = {
    .name          = TYPE_BIONZ_MISCCTRL,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(MiscctrlState),
    .class_init    = miscctrl_class_init,
};

static void miscctrl_register_type(void)
{
    type_register_static(&miscctrl_info);
}

type_init(miscctrl_register_type)
