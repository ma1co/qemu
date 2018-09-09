/* QEMU model of the Sony BIONZ nand controller */

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "qemu/log.h"
#include "sysemu/block-backend.h"

#define NAND_PAGE_SIZE 0x600

#define TYPE_BIONZ_NAND "bionz_nand"
#define BIONZ_NAND(obj) OBJECT_CHECK(NandState, (obj), TYPE_BIONZ_NAND)

typedef struct NandState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;

    BlockBackend *blk;

    uint32_t offset;
} NandState;

static uint64_t nand_read(void *opaque, hwaddr offset, unsigned size)
{
    NandState *s = BIONZ_NAND(opaque);
    uint64_t value;

    switch (offset) {
        case 0x10:
            if (s->blk && blk_pread(s->blk, s->offset, &value, size) < 0) {
                hw_error("%s: Cannot read block device\n", __func__);
            }
            s->offset += size;
            return value;

        default:
            qemu_log_mask(LOG_UNIMP, "%s: unimplemented read @ 0x%" HWADDR_PRIx "\n", __func__, offset);
            return 0;
    }
}

static void nand_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    NandState *s = BIONZ_NAND(opaque);

    switch (offset) {
        case 0x0:
            s->offset = (value & ~0xc0000000) * NAND_PAGE_SIZE;
            break;

        default:
            qemu_log_mask(LOG_UNIMP, "%s: unimplemented write @ 0x%" HWADDR_PRIx ": 0x%" PRIx64 "\n", __func__, offset, value);
    }
}

static const struct MemoryRegionOps nand_ops = {
    .read = nand_read,
    .write = nand_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void nand_reset(DeviceState *dev)
{
    NandState *s = BIONZ_NAND(dev);
    s->offset = 0;
}

static int nand_init(SysBusDevice *sbd)
{
    NandState *s = BIONZ_NAND(sbd);
    memory_region_init_io(&s->mmio, OBJECT(sbd), &nand_ops, s, TYPE_BIONZ_NAND, 0x10000);
    sysbus_init_mmio(sbd, &s->mmio);
    return 0;
}

static Property nand_properties[] = {
    DEFINE_PROP_DRIVE("drive", NandState, blk),
    DEFINE_PROP_END_OF_LIST(),
};

static void nand_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SysBusDeviceClass *k = SYS_BUS_DEVICE_CLASS(klass);

    k->init = nand_init;
    dc->reset = nand_reset;
    dc->props = nand_properties;
}

static const TypeInfo nand_info = {
    .name          = TYPE_BIONZ_NAND,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(NandState),
    .class_init    = nand_class_init,
};

static void nand_register_type(void)
{
    type_register_static(&nand_info);
}

type_init(nand_register_type)
