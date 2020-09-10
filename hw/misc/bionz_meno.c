/* QEMU model of the Sony BIONZ onenand coprocessor (meno) */

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/lz77.h"
#include "sysemu/block-backend.h"

#define PHYS_ADDR(addr) ((addr) - 0x10000000)

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
    qemu_irq intr;

    void *blk;

    uint32_t csr;
    uint32_t poll_mode;
} MenoState;

typedef struct MenoReadArgs {
    uint32_t block;
    uint32_t sector;
    uint32_t num_buffers;
    uint32_t buffer_ptr;
    uint32_t size_ptr;
} MenoReadArgs;

typedef struct MenoLzReadArgs {
    uint32_t num;
    uint32_t block_ptr;
    uint32_t sector_ptr;
    uint32_t num_sector_ptr;
    uint32_t offset;
    uint32_t block_size;
    uint32_t buffer;
} MenoLzReadArgs;

static void meno_update_irq(MenoState *s)
{
    qemu_set_irq(s->intr, !s->poll_mode && s->csr);
}

static void meno_nand_read(MenoState *s, uint32_t args_ptr, uint32_t offset, uint32_t sector_size)
{
    MenoReadArgs args;
    uint32_t buffer_ptr;
    uint32_t size;
    void *buffer;
    int i;

    cpu_physical_memory_read(PHYS_ADDR(args_ptr), &args, sizeof(args));
    offset += (args.block * NAND_SECTORS_PER_BLOCK + args.sector) * sector_size;

    for (i = 0; i < args.num_buffers; i++) {
        cpu_physical_memory_read(PHYS_ADDR(args.buffer_ptr + i * sizeof(buffer_ptr)), &buffer_ptr, sizeof(buffer_ptr));
        cpu_physical_memory_read(PHYS_ADDR(args.size_ptr + i * sizeof(size)), &size, sizeof(size));

        buffer = g_malloc(size);
        if (s->blk && blk_pread((BlockBackend *) s->blk, offset, buffer, size) < 0) {
            hw_error("%s: Cannot read block device\n", __func__);
        }
        cpu_physical_memory_write(PHYS_ADDR(buffer_ptr), buffer, size);
        g_free(buffer);

        offset += size;
    }
}

static void meno_nand_lz_read(MenoState *s, uint32_t args_ptr)
{
    MenoLzReadArgs args;
    uint32_t block, sector, num_sector;
    unsigned int src_size, dst_size, off;
    unsigned char *src_buffer, *dst_buffer, *src, *dst;
    int i, res;

    cpu_physical_memory_read(PHYS_ADDR(args_ptr), &args, sizeof(args));

    src_size = 0;
    for (i = 0; i < args.num; i++) {
        cpu_physical_memory_read(PHYS_ADDR(args.num_sector_ptr + i * sizeof(num_sector)), &num_sector, sizeof(num_sector));
        src_size += num_sector * NAND_SECTOR_SIZE;
    }
    dst_size = 1 << args.block_size;

    src_buffer = g_malloc(src_size);
    dst_buffer = g_malloc(dst_size);

    src = src_buffer;
    for (i = 0; i < args.num; i++) {
        cpu_physical_memory_read(PHYS_ADDR(args.block_ptr + i * sizeof(block)), &block, sizeof(block));
        cpu_physical_memory_read(PHYS_ADDR(args.sector_ptr + i * sizeof(sector)), &sector, sizeof(sector));
        cpu_physical_memory_read(PHYS_ADDR(args.num_sector_ptr + i * sizeof(num_sector)), &num_sector, sizeof(num_sector));
        off = (block * NAND_SECTORS_PER_BLOCK + sector) * NAND_SECTOR_SIZE;
        if (s->blk && blk_pread((BlockBackend *) s->blk, off, src, num_sector * NAND_SECTOR_SIZE) < 0) {
            hw_error("%s: Cannot read block device\n", __func__);
        }
        src += num_sector * NAND_SECTOR_SIZE;
    }

    src = src_buffer + args.offset;
    dst = dst_buffer;
    while (src < src_buffer + src_size && dst < dst_buffer + dst_size) {
        res = lz77_inflate(src, src_buffer + src_size - src, dst, dst_buffer + dst_size - dst, &src);
        if (res < 0) {
            hw_error("%s: lz77_inflate failed\n", __func__);
        }
        dst += res;
    }

    cpu_physical_memory_write(PHYS_ADDR(args.buffer), dst_buffer, dst_size);

    g_free(src_buffer);
    g_free(dst_buffer);
}

static void meno_command(MenoState *s)
{
    uint32_t args_ptr, action;
    void *fwram = memory_region_get_ram_ptr(&s->fwram);

    cpu_physical_memory_read(PHYS_ADDR(*(uint32_t *)(fwram + 0x18d4)), &args_ptr, sizeof(args_ptr));
    cpu_physical_memory_read(PHYS_ADDR(args_ptr), &action, sizeof(action));
    args_ptr += 0x14;

    switch (action) {
        case 1:
            meno_nand_read(s, args_ptr, 0, NAND_SECTOR_SIZE);
            break;

        case 2:
            meno_nand_read(s, args_ptr, NAND_NUM_BLOCKS * NAND_SECTORS_PER_BLOCK * NAND_SECTOR_SIZE, NAND_SPARE_SIZE);
            break;

        case 12:
            meno_nand_lz_read(s, args_ptr);
            break;

        default:
            qemu_log_mask(LOG_UNIMP, "%s: unimplemented command %d\n", __func__, action);
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
            qemu_log_mask(LOG_UNIMP, "%s: unimplemented write @ 0x%" HWADDR_PRIx ": 0x%" PRIx64 "\n", __func__, offset, value);
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

static void meno_realize(DeviceState *dev, Error **errp)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    MenoState *s = BIONZ_MENO(dev);

    memory_region_init(&s->container, OBJECT(dev), TYPE_BIONZ_MENO, 0x3000);
    sysbus_init_mmio(sbd, &s->container);

    memory_region_init_io(&s->mmio, OBJECT(dev), &meno_ops, s, TYPE_BIONZ_MENO ".mmio", 0x1000);
    memory_region_add_subregion(&s->container, 0, &s->mmio);

    /* The firmware is written to the fwram by the device driver. The firmware is ignored by this model. */
    memory_region_init_ram_nomigrate(&s->fwram, OBJECT(dev), TYPE_BIONZ_MENO ".fwram", 0x2000, &error_fatal);
    memory_region_add_subregion(&s->container, 0x1000, &s->fwram);

    sysbus_init_irq(sbd, &s->intr);
}

static Property meno_properties[] = {
    DEFINE_PROP_PTR("drive_ptr", MenoState, blk),
    DEFINE_PROP_END_OF_LIST(),
};

static void meno_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = meno_realize;
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
