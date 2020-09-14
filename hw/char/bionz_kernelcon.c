#include "qemu/osdep.h"
#include "chardev/char-fe.h"
#include "hw/sysbus.h"
#include "qemu/log.h"

#define TYPE_BIONZ_KERNELCON "bionz_kernelcon"
#define BIONZ_KERNELCON(obj) OBJECT_CHECK(KernelconState, (obj), TYPE_BIONZ_KERNELCON)

typedef struct KernelconState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    CharBackend chr;
} KernelconState;

static uint64_t kernelcon_read(void *opaque, hwaddr offset, unsigned size)
{
    qemu_log_mask(LOG_UNIMP, "%s: unimplemented read\n", __func__);
    return 0;
}

static void kernelcon_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    KernelconState *s = BIONZ_KERNELCON(opaque);
    qemu_chr_fe_write_all(&s->chr, (void *) &value, size);
}

static const struct MemoryRegionOps kernelcon_ops = {
    .read = kernelcon_read,
    .write = kernelcon_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static int kernelcon_init(SysBusDevice *sbd)
{
    KernelconState *s = BIONZ_KERNELCON(sbd);
    memory_region_init_io(&s->mmio, OBJECT(sbd), &kernelcon_ops, s, TYPE_BIONZ_KERNELCON, 1 << 13);
    sysbus_init_mmio(sbd, &s->mmio);
    return 0;
}

static Property kernelcon_properties[] = {
    DEFINE_PROP_CHR("chardev", KernelconState, chr),
    DEFINE_PROP_END_OF_LIST(),
};

static void kernelcon_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SysBusDeviceClass *k = SYS_BUS_DEVICE_CLASS(klass);
    k->init = kernelcon_init;
    dc->props = kernelcon_properties;
}

static const TypeInfo kernelcon_info = {
    .name          = TYPE_BIONZ_KERNELCON,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(KernelconState),
    .class_init    = kernelcon_class_init,
};

static void kernelcon_register_type(void)
{
    type_register_static(&kernelcon_info);
}

type_init(kernelcon_register_type)
