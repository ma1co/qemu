/* QEMU model of the Sony BIONZ onenand coprocessor (meno) */

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "sysemu/block-backend.h"

#define AS_ADDR(addr) ((addr) - 0x10000000)

#define NAND_SECTOR_SIZE 0x200
#define NAND_SPARE_SIZE 0x10
#define NAND_SECTORS_PER_BLOCK 0x100
#define NAND_NUM_BLOCKS 0x800

#define TYPE_BIONZ_MENO "bionz_meno"
#define BIONZ_MENO(obj) OBJECT_CHECK(MenoState, (obj), TYPE_BIONZ_MENO)

typedef struct MenoState {
    SysBusDevice parent_obj;
    MemoryRegion container;
    MemoryRegion mmio;
    MemoryRegion fwram;
    AddressSpace as;
    qemu_irq intr;

    void *blk;

    uint32_t csr;
    uint32_t poll_mode;
} MenoState;

typedef struct MenoArgs {
    uint32_t action;
    uint32_t pad[4];
    uint32_t block;
    uint32_t sector;
    uint32_t num_buffers;
    uint32_t buffer_ptr;
    uint32_t size_ptr;
} MenoArgs;

static void meno_as_read(MenoState *s, uint32_t addr, void *buf, uint32_t len)
{
    if (address_space_read(&s->as, AS_ADDR(addr), MEMTXATTRS_UNSPECIFIED, buf, len) != MEMTX_OK) {
        hw_error("%s: cannot read from 0x%" PRIx32 "\n", __func__, addr);
    }
}

static void meno_as_write(MenoState *s, uint32_t addr, void *buf, uint32_t len)
{
    if (address_space_write(&s->as, AS_ADDR(addr), MEMTXATTRS_UNSPECIFIED, buf, len) != MEMTX_OK) {
        hw_error("%s: cannot write to 0x%" PRIx32 "\n", __func__, addr);
    }
}

static void meno_update_irq(MenoState *s)
{
    qemu_set_irq(s->intr, !s->poll_mode && s->csr);
}

static void meno_nand_read(MenoState *s, MenoArgs *args, uint32_t offset)
{
    uint32_t buffer_ptr;
    uint32_t size;
    void *buffer;
    int i;

    for (i = 0; i < args->num_buffers; i++) {
        meno_as_read(s, args->buffer_ptr + i * sizeof(buffer_ptr), &buffer_ptr, sizeof(buffer_ptr));
        meno_as_read(s, args->size_ptr + i * sizeof(size), &size, sizeof(size));

        buffer = g_malloc(size);
        if (s->blk && blk_pread((BlockBackend *) s->blk, offset, buffer, size) < 0)
            hw_error("%s: Cannot read block device\n", __func__);
        meno_as_write(s, buffer_ptr, buffer, size);
        g_free(buffer);

        offset += size;
    }
}

static void meno_command(MenoState *s)
{
    uint32_t args_ptr;
    MenoArgs args;
    void *fwram = memory_region_get_ram_ptr(&s->fwram);

    meno_as_read(s, *(uint32_t *)(fwram + 0x18d4), &args_ptr, sizeof(args_ptr));
    meno_as_read(s, args_ptr, &args, sizeof(args));

    switch (args.action) {
        case 1:
            meno_nand_read(s, &args, (args.block * NAND_SECTORS_PER_BLOCK + args.sector) * NAND_SECTOR_SIZE);
            break;

        case 2:
            meno_nand_read(s, &args, NAND_NUM_BLOCKS * NAND_SECTORS_PER_BLOCK * NAND_SECTOR_SIZE
                                     + (args.block * NAND_SECTORS_PER_BLOCK + args.sector) * NAND_SPARE_SIZE);
            break;

        default:
            qemu_log_mask(LOG_UNIMP, "%s: unimplemented command %d\n", __func__, args.action);
    }

    s->csr = 1;
    meno_update_irq(s);
}

static uint64_t meno_read(void *opaque, hwaddr offset, unsigned size)
{
    MenoState *s = BIONZ_MENO(opaque);

    switch (offset) {
        case 0x0f84:
            return 0;

        case 0x0f88:
            return s->csr;

        case 0x0f8c:
            return s->poll_mode;

        default:
            qemu_log_mask(LOG_UNIMP, "%s: unimplemented read @ 0x%" HWADDR_PRIx "\n", __func__, offset);
            return 0;
    }
}

static void meno_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    MenoState *s = BIONZ_MENO(opaque);

    switch (offset) {
        case 0x0f84:
            if (value & 1) {
                meno_command(s);
            }
            break;

        case 0x0f88:
            s->csr = value;
            meno_update_irq(s);
            break;

        case 0x0f8c:
            s->poll_mode = value;
            meno_update_irq(s);
            break;

        default:
            qemu_log_mask(LOG_UNIMP, "%s: unimplemented  write @ 0x%" HWADDR_PRIx ": 0x%" PRIx64 "\n", __func__, offset, value);
    }
}

static const struct MemoryRegionOps meno_ops = {
    .read = meno_read,
    .write = meno_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void meno_reset(DeviceState *dev)
{
    MenoState *s = BIONZ_MENO(dev);
    s->csr = 0;
    s->poll_mode = 0;
}

static int meno_init(SysBusDevice *sbd)
{
    MenoState *s = BIONZ_MENO(sbd);

    address_space_init(&s->as, sysbus_address_space(sbd), "as");

    memory_region_init(&s->container, OBJECT(sbd), TYPE_BIONZ_MENO, 0x3000);
    sysbus_init_mmio(sbd, &s->container);

    memory_region_init_io(&s->mmio, OBJECT(sbd), &meno_ops, s, TYPE_BIONZ_MENO ".mmio", 0x1000);
    memory_region_add_subregion(&s->container, 0, &s->mmio);

    /* The firmware is written to the fwram by the device driver. The firmware is ignored by this model. */
    memory_region_init_ram_nomigrate(&s->fwram, OBJECT(sbd), TYPE_BIONZ_MENO ".fwram", 0x2000, &error_fatal);
    memory_region_add_subregion(&s->container, 0x1000, &s->fwram);

    sysbus_init_irq(sbd, &s->intr);

    return 0;
}

static Property meno_properties[] = {
    DEFINE_PROP_PTR("drive_ptr", MenoState, blk),
    DEFINE_PROP_END_OF_LIST(),
};

static void meno_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SysBusDeviceClass *k = SYS_BUS_DEVICE_CLASS(klass);

    k->init = meno_init;
    dc->reset = meno_reset;
    dc->props = meno_properties;
}

static const TypeInfo meno_info = {
    .name          = TYPE_BIONZ_MENO,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(MenoState),
    .class_init    = meno_class_init,
};

static void meno_register_type(void)
{
    type_register_static(&meno_info);
}

type_init(meno_register_type)
