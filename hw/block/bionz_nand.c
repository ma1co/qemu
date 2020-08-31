/* QEMU model of the Sony BIONZ nand controller (similar to denali flash controller, but with a custom dma interface) */

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "sysemu/block-backend.h"

#define NAND_PAGE_SIZE 0x1000
#define NAND_SPARE_SIZE 8

#define COMMAND_IRQ_DELAY 100000

#define REG_GLOBAL_INT_ENABLE        0x0f0
#define REG_NUMBER_OF_PLANES         0x140
#define REG_PAGES_PER_BLOCK          0x150
#define REG_MAIN_AREA_SIZE           0x170
#define REG_SPARE_AREA_SIZE          0x180
#define REG_FIRST_BLOCK_OF_NEXT_PANE 0x270
#define REG_INTR_STATUS0             0x410
#define REG_INTR_EN0                 0x420
#define REG_DMA_ENABLE               0x700
#define REG_DMA_INTR                 0x720
#define REG_DMA_INTR_EN              0x730

#define DATA_CTRL 0x00
#define DATA_DATA 0x10

#define INTR_LOAD_COMP  (1 << 6)
#define INTR_ERASE_COMP (1 << 8)
#define INTR_RST_COMP   (1 << 13)

#define TYPE_BIONZ_NAND "bionz_nand"
#define BIONZ_NAND(obj) OBJECT_CHECK(NandState, (obj), TYPE_BIONZ_NAND)

typedef struct NandDmaArgs {
    uint32_t unknown0[2];
    uint32_t command;
    uint32_t unknown1[1];
    uint32_t data;
    uint32_t unknown2[1];
    uint32_t main_buffer;
    uint32_t unknown3[1];
    uint32_t result;
    uint32_t unknown4[5];
    uint32_t spare_buffer;
    uint32_t unknown5[1];
} NandDmaArgs;

typedef struct NandState {
    SysBusDevice parent_obj;
    MemoryRegion reg_mmio;
    MemoryRegion data_mmio;
    qemu_irq intr;
    QEMUTimer *update_irq_timer;

    BlockBackend *blk;
    uint32_t size;

    uint32_t ctrl;
    uint32_t offset;

    uint32_t reg_global_int_enable;
    uint32_t reg_number_of_planes;
    uint32_t reg_pages_per_block;
    uint32_t reg_main_area_size;
    uint32_t reg_spare_area_size;
    uint32_t reg_first_block_of_next_pane;
    uint32_t reg_intr_status0;
    uint32_t reg_intr_en0;
    uint32_t reg_dma_enable;
    uint32_t reg_dma_intr;
    uint32_t reg_dma_intr_en;

    uint32_t dma_args[3];
    unsigned int dma_arg_count;
} NandState;

static void nand_update_irq(NandState *s)
{
    qemu_set_irq(s->intr, (s->reg_global_int_enable & 1) && ((s->reg_intr_en0 & s->reg_intr_status0) || (s->reg_dma_intr_en & s->reg_dma_intr)));
}

static void nand_update_irq_delayed(void *opaque)
{
    nand_update_irq(BIONZ_NAND(opaque));
}

static uint64_t nand_reg_read(void *opaque, hwaddr offset, unsigned size)
{
    NandState *s = BIONZ_NAND(opaque);

    switch (offset) {
        case REG_GLOBAL_INT_ENABLE:
            return s->reg_global_int_enable;

        case REG_NUMBER_OF_PLANES:
            return s->reg_number_of_planes;

        case REG_PAGES_PER_BLOCK:
            return s->reg_pages_per_block;

        case REG_MAIN_AREA_SIZE:
            return s->reg_main_area_size;

        case REG_SPARE_AREA_SIZE:
            return s->reg_spare_area_size;

        case REG_FIRST_BLOCK_OF_NEXT_PANE:
            return s->reg_first_block_of_next_pane;

        case REG_INTR_STATUS0:
            return s->reg_intr_status0 | INTR_RST_COMP;

        case REG_INTR_EN0:
            return s->reg_intr_en0;

        case REG_DMA_ENABLE:
            return s->reg_dma_enable;

        case REG_DMA_INTR:
            return s->reg_dma_intr;

        case REG_DMA_INTR_EN:
            return s->reg_dma_intr_en;

        default:
            qemu_log_mask(LOG_UNIMP, "%s: unimplemented read @ 0x%" HWADDR_PRIx "\n", __func__, offset);
            return 0;
    }
}

static void nand_reg_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    NandState *s = BIONZ_NAND(opaque);

    switch (offset) {
        case REG_GLOBAL_INT_ENABLE:
            s->reg_global_int_enable = value;
            nand_update_irq(s);
            break;

        case REG_NUMBER_OF_PLANES:
            s->reg_number_of_planes = value;
            break;

        case REG_PAGES_PER_BLOCK:
            s->reg_pages_per_block = value;
            break;

        case REG_MAIN_AREA_SIZE:
            s->reg_main_area_size = value;
            break;

        case REG_SPARE_AREA_SIZE:
            s->reg_spare_area_size = value;
            break;

        case REG_FIRST_BLOCK_OF_NEXT_PANE:
            s->reg_first_block_of_next_pane = value;
            break;

        case REG_INTR_STATUS0:
            s->reg_intr_status0 &= ~value;
            nand_update_irq(s);
            break;

        case REG_INTR_EN0:
            s->reg_intr_en0 = value;
            nand_update_irq(s);
            break;

        case REG_DMA_ENABLE:
            s->reg_dma_enable = value;
            if (!value) {
                s->dma_arg_count = 0;
            }
            break;

        case REG_DMA_INTR:
            s->reg_dma_intr &= ~value;
            nand_update_irq(s);
            break;

        case REG_DMA_INTR_EN:
            s->reg_dma_intr_en = value;
            nand_update_irq(s);
            break;

        default:
            qemu_log_mask(LOG_UNIMP, "%s: unimplemented write @ 0x%" HWADDR_PRIx ": 0x%" PRIx64 "\n", __func__, offset, value);
    }
}

static const struct MemoryRegionOps nand_reg_ops = {
    .read = nand_reg_read,
    .write = nand_reg_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void nand_dma_command(NandState *s)
{
    NandDmaArgs args;
    uint32_t main_offset, main_len, spare_offset, spare_len;
    void *buffer;

    if (s->dma_args[0] != 0x80 || s->dma_args[2] != 0) {
        hw_error("%s: Invalid arguments: 0x%x, 0x%x, 0x%x\n", __func__, s->dma_args[0], s->dma_args[1], s->dma_args[2]);
    }

    cpu_physical_memory_read(s->dma_args[1], &args, sizeof(args));

    if (((args.command >> 26) & 3) != 0b10) {// MAP10
        hw_error("%s: Invalid command: 0x%x\n", __func__, args.command);
    }

    switch ((args.data >> 8) & 0xff) {
        case 0x20:// read
            main_offset = (args.command & 0xffffff) * NAND_PAGE_SIZE;
            spare_offset = s->size + (args.command & 0xffffff) * NAND_SPARE_SIZE;

            switch (args.data >> 16) {
                case 0x3140:// 0x1000 byte pages
                    main_len = (args.data & 0xff) * NAND_PAGE_SIZE;
                    spare_len = (args.data & 0xff) * NAND_SPARE_SIZE;
                    break;

                case 0x5140:// 0x200 byte sectors
                    main_offset += NAND_PAGE_SIZE / 8 * ((args.data >> 4) & 7);
                    main_len = NAND_PAGE_SIZE / 8 * (args.data & 7);
                    spare_len = NAND_SPARE_SIZE;
                    break;

                default:
                    hw_error("%s: Invalid read mode: 0x%x\n", __func__, args.data >> 16);
            }

            buffer = g_malloc(main_len);
            if (s->blk && blk_pread(s->blk, main_offset, buffer, main_len) < 0) {
                hw_error("%s: Cannot read block device\n", __func__);
            }
            cpu_physical_memory_write(args.main_buffer, buffer, main_len);
            g_free(buffer);

            buffer = g_malloc(spare_len);
            if (s->blk && blk_pread(s->blk, spare_offset, buffer, spare_len) < 0) {
                hw_error("%s: Cannot read block device\n", __func__);
            }
            cpu_physical_memory_write(args.spare_buffer, buffer, spare_len);
            g_free(buffer);
            break;

        case 0x21:// write
            break;

        default:
            hw_error("%s: Invalid data: 0x%x\n", __func__, args.data);
    }

    args.result = 0x8000;
    cpu_physical_memory_write(s->dma_args[1], &args, sizeof(args));

    s->reg_dma_intr |= (1 << 1);
    nand_update_irq(s);
}

static uint64_t nand_data_read(void *opaque, hwaddr offset, unsigned size)
{
    NandState *s = BIONZ_NAND(opaque);
    uint64_t value;

    switch (offset) {
        case DATA_DATA:
            switch ((s->ctrl >> 26) & 3) {
                case 0b01:// MAP01
                    if (s->blk && blk_pread(s->blk, s->offset, &value, size) < 0) {
                        hw_error("%s: Cannot read block device\n", __func__);
                    }
                    s->offset += size;
                    return value;
            }
            break;
    }

    qemu_log_mask(LOG_UNIMP, "%s: unimplemented read @ 0x%" HWADDR_PRIx "\n", __func__, offset);
    return 0;
}

static void nand_data_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    NandState *s = BIONZ_NAND(opaque);

    switch (offset) {
        case DATA_CTRL:
            s->ctrl = value;
            switch ((s->ctrl >> 26) & 3) {
                case 0b01:// MAP01
                    s->offset = (s->ctrl & 0xffffff) * NAND_PAGE_SIZE;
                    break;
            }
            return;

        case DATA_DATA:
            if (s->reg_dma_enable & 1) {
                s->dma_args[s->dma_arg_count++] = value;
                if (s->dma_arg_count == ARRAY_SIZE(s->dma_args)) {
                    nand_dma_command(s);
                    s->dma_arg_count = 0;
                }
                return;
            } else {
                switch ((s->ctrl >> 26) & 3) {
                    case 0b10:// MAP10
                        if (value == 1) {// erase
                            s->reg_intr_status0 |= INTR_ERASE_COMP;
                        } else if ((value >> 8) == 0x20) {// pipeline read-ahead
                            s->reg_intr_status0 |= INTR_LOAD_COMP;
                        } else {
                            break;
                        }
                        timer_mod(s->update_irq_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + COMMAND_IRQ_DELAY);
                        return;
                }
            }
            break;
    }

    qemu_log_mask(LOG_UNIMP, "%s: unimplemented write @ 0x%" HWADDR_PRIx ": 0x%" PRIx64 "\n", __func__, offset, value);
}

static const struct MemoryRegionOps nand_data_ops = {
    .read = nand_data_read,
    .write = nand_data_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void nand_reset(DeviceState *dev)
{
    NandState *s = BIONZ_NAND(dev);
    int i;

    s->ctrl = 0;
    s->offset = 0;

    s->reg_global_int_enable = 0;
    s->reg_number_of_planes = 0;
    s->reg_pages_per_block = 0;
    s->reg_main_area_size = 0;
    s->reg_spare_area_size = 0;
    s->reg_first_block_of_next_pane = 0;
    s->reg_intr_status0 = 0;
    s->reg_intr_en0 = 0;
    s->reg_dma_enable = 0;
    s->reg_dma_intr = 0;
    s->reg_dma_intr_en = 0;

    for (i = 0; i < ARRAY_SIZE(s->dma_args); i++) {
        s->dma_args[i] = 0;
    }
    s->dma_arg_count = 0;

    timer_del(s->update_irq_timer);
}

static int nand_init(SysBusDevice *sbd)
{
    uint32_t length;
    NandState *s = BIONZ_NAND(sbd);

    if (!s->size && s->blk) {
        length = blk_getlength(s->blk);
        s->size = (length / (NAND_PAGE_SIZE + NAND_SPARE_SIZE)) * NAND_PAGE_SIZE;
        if (s->size / NAND_PAGE_SIZE * (NAND_PAGE_SIZE + NAND_SPARE_SIZE) != length) {
            error_report("Can't determine size from drive");
            return -1;
        }
    }

    memory_region_init_io(&s->reg_mmio, OBJECT(sbd), &nand_reg_ops, s, TYPE_BIONZ_NAND ".reg", 0x800);
    sysbus_init_mmio(sbd, &s->reg_mmio);

    memory_region_init_io(&s->data_mmio, OBJECT(sbd), &nand_data_ops, s, TYPE_BIONZ_NAND ".data", 0x20);
    sysbus_init_mmio(sbd, &s->data_mmio);

    sysbus_init_irq(sbd, &s->intr);

    s->update_irq_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, nand_update_irq_delayed, s);

    return 0;
}

static Property nand_properties[] = {
    DEFINE_PROP_DRIVE("drive", NandState, blk),
    DEFINE_PROP_UINT32("size", NandState, size, 0),
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
