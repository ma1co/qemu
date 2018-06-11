/* QEMU model of the Sony BIONZ CXD4132 image processor */

#include "qemu/osdep.h"
#include "cpu.h"
#include "hw/sysbus.h"
#include "hw/block/flash.h"
#include "hw/boards.h"
#include "hw/char/pl011.h"
#include "hw/cpu/arm11mpcore.h"
#include "hw/loader.h"
#include "qapi/error.h"
#include "sysemu/block-backend.h"
#include "sysemu/sysemu.h"

#define CXD4132_NAND_BASE 0x00000000

#define CXD4132_DDR_BASE 0x80000000
#define CXD4132_DDR_SIZE 0x20000000

#define CXD4132_SRAM_BASE 0xa0000000
#define CXD4132_SRAM_SIZE 0x00400000

#define CXD4132_DMA_BASE 0xf2001000

#define CXD4132_MENO_BASE 0xf2002000

#define CXD4132_HWTIMER_BASE(i) (0xf2008000 + (i) * 0x20)
#define CXD4132_NUM_HWTIMER 5

#define CXD4132_UART_BASE(i) (0xf2038000 + (i) * 0x1000)
#define CXD4132_NUM_UART 3

#define CXD4132_MISCCTRL_BASE 0xf3060000

#define CXD4132_MPCORE_BASE 0xf8000000

#define CXD4132_IRQ_UART(i) (160 + (i))
#define CXD4132_IRQ_HWTIMER(i) (163 + (i))
#define CXD4132_IRQ_DMA(i) (176 + (i))
#define CXD4132_IRQ_MENO 180
#define CXD4132_IRQ_NAND 183

#define CXD4132_NUM_IRQ 256
#define CXD4132_IRQ_OFFSET 32

#define CXD4132_CMDLINE_OFFSET 0x00013000
#define CXD4132_TEXT_OFFSET 0x00018000
#define CXD4132_INITRD_OFFSET 0x00408000

#define NAND_SECTOR_SIZE 0x200

#define TYPE_CXD4132 MACHINE_TYPE_NAME("cxd4132")
#define CXD4132(obj) OBJECT_CHECK(Cxd4132State, (obj), TYPE_CXD4132)

#define CXD4132_CMDLINE \
    "lpj=622592 " \
    "console=ttyAM0,115200n8 " \
    "amba2.console=1 " \
    "ip=off " \
    "initrd=0x80408000,0x00700000 " \
    "root=/dev/ram0 " \
    "boottime=0x20000@0x833C0000 " \
    "klog.size=0x20000 " \
    "klog.addr=0x833E0080 " \
    "mem=64M@0x80000000@0 " \
    "memrsv=32K@0x80000000 " \
    "memrsv=0x1270000@0x82D90000 "

typedef struct Cxd4132State {
    MachineState parent_obj;
    ARMCPU cpu;
    BlockBackend *drive;
    hwaddr loader_base;
} Cxd4132State;

static void cxd4132_reset(void *opaque)
{
    Cxd4132State *s = CXD4132(opaque);
    cpu_reset(CPU(&s->cpu));
    cpu_set_pc(CPU(&s->cpu), s->loader_base);
}

static uint32_t cxd4132_init_loader(Cxd4132State *s)
{
    char boot_block[NAND_SECTOR_SIZE];
    uint32_t loader_offset, loader_size, loader_base;
    void *loader_buffer;

    if (!s->drive) {
        return 0;
    }

    if (blk_pread(s->drive, 0, boot_block, sizeof(boot_block)) < 0) {
        hw_error("%s: Cannot read boot block\n", __func__);
    }

    if (*(uint32_t *) boot_block != *(uint32_t *) "EXBL") {
        hw_error("%s: Wrong boot block signature\n", __func__);
    }

    loader_offset = (*(uint32_t *) (boot_block + 0x40)) * NAND_SECTOR_SIZE;
    loader_size = (*(uint32_t *) (boot_block + 0x44)) * NAND_SECTOR_SIZE;
    loader_base = *(uint32_t *) (boot_block + 0x50);

    loader_buffer = g_malloc(loader_size);
    if (blk_pread(s->drive, loader_offset, loader_buffer, loader_size) < 0) {
        hw_error("%s: Cannot read loader\n", __func__);
    }
    rom_add_blob_fixed("loader", loader_buffer, loader_size, loader_base);
    g_free(loader_buffer);

    return loader_base;
}

static void cxd4132_init_cmdline(const char *default_cmdline, const char *cmdline, hwaddr base)
{
    const char *header = "kemco ";
    const char *spacer = " ";
    const char *footer = " *";
    const size_t len = strlen(header) + strlen(default_cmdline) + strlen(spacer) + strlen(cmdline) + strlen(footer);
    char *buf = g_malloc(len + 1);
    strcpy(buf, header);
    strcat(buf, default_cmdline);
    strcat(buf, spacer);
    strcat(buf, cmdline);
    strcat(buf, footer);
    rom_add_blob_fixed("cmdline", buf, len, base);
    g_free(buf);
}

static void cxd4132_init(MachineState *machine)
{
    Cxd4132State *s = CXD4132(machine);
    DriveInfo *dinfo;
    MemoryRegion *mem;
    DeviceState *dev;
    qemu_irq irq[CXD4132_NUM_IRQ - CXD4132_IRQ_OFFSET];
    int i;

    dinfo = drive_get(IF_MTD, 0, 0);
    s->drive = dinfo ? blk_by_legacy_dinfo(dinfo) : NULL;

    object_initialize(&s->cpu, sizeof(s->cpu), machine->cpu_type);
    qdev_init_nofail(DEVICE(&s->cpu));

    mem = g_new(MemoryRegion, 1);
    memory_region_init_ram(mem, NULL, "ddr", CXD4132_DDR_SIZE, &error_fatal);
    memory_region_add_subregion(get_system_memory(), CXD4132_DDR_BASE, mem);

    mem = g_new(MemoryRegion, 1);
    memory_region_init_ram(mem, NULL, "sram", CXD4132_SRAM_SIZE, &error_fatal);
    memory_region_add_subregion(get_system_memory(), CXD4132_SRAM_BASE, mem);

    dev = qdev_create(NULL, TYPE_ARM11MPCORE_PRIV);
    qdev_prop_set_uint32(dev, "num-cpu", 1);
    qdev_prop_set_uint32(dev, "num-irq", CXD4132_NUM_IRQ);
    qdev_init_nofail(dev);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, CXD4132_MPCORE_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, qdev_get_gpio_in(DEVICE(&s->cpu), ARM_CPU_IRQ));
    for (i = 0; i < CXD4132_NUM_IRQ - CXD4132_IRQ_OFFSET; i++) {
        irq[i] = qdev_get_gpio_in(dev, i);
    }

    dev = qdev_create(NULL, "onenand");
    qdev_prop_set_uint16(dev, "manufacturer_id", NAND_MFR_SAMSUNG);
    qdev_prop_set_uint16(dev, "device_id", 0x40);
    qdev_prop_set_int32(dev, "shift", 1);
    qdev_prop_set_drive(dev, "drive", s->drive, &error_fatal);
    qdev_init_nofail(dev);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, CXD4132_NAND_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, irq[CXD4132_IRQ_NAND - CXD4132_IRQ_OFFSET]);

    dev = qdev_create(NULL, "bionz_dma");
    qdev_init_nofail(dev);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, CXD4132_DMA_BASE);
    for (i = 0; i < 4; i++) {
        sysbus_connect_irq(SYS_BUS_DEVICE(dev), i, irq[CXD4132_IRQ_DMA(i) - CXD4132_IRQ_OFFSET]);
    }

    dev = qdev_create(NULL, "bionz_meno");
    qdev_prop_set_ptr(dev, "drive_ptr", s->drive);
    qdev_init_nofail(dev);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, CXD4132_MENO_BASE);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, irq[CXD4132_IRQ_MENO - CXD4132_IRQ_OFFSET]);

    for (i = 0; i < CXD4132_NUM_HWTIMER; i++) {
        dev = qdev_create(NULL, "bionz_hwtimer");
        qdev_init_nofail(dev);
        sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, CXD4132_HWTIMER_BASE(i));
        sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0, irq[CXD4132_IRQ_HWTIMER(i) - CXD4132_IRQ_OFFSET]);
    }

    for (i = 0; i < CXD4132_NUM_UART; i++) {
        pl011_create(CXD4132_UART_BASE(i), irq[CXD4132_IRQ_UART(i) - CXD4132_IRQ_OFFSET], serial_hds[i]);
    }

    dev = qdev_create(NULL, "bionz_miscctrl");
    qdev_prop_set_uint32(dev, "typeid", 0x301);
    qdev_init_nofail(dev);
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, CXD4132_MISCCTRL_BASE);

    if (machine->kernel_filename) {
        load_image_targphys(machine->kernel_filename, CXD4132_DDR_BASE + CXD4132_TEXT_OFFSET, CXD4132_DDR_SIZE - CXD4132_TEXT_OFFSET);
        load_image_targphys(machine->initrd_filename, CXD4132_DDR_BASE + CXD4132_INITRD_OFFSET, CXD4132_DDR_SIZE - CXD4132_INITRD_OFFSET);
        cxd4132_init_cmdline(CXD4132_CMDLINE, machine->kernel_cmdline, CXD4132_DDR_BASE + CXD4132_CMDLINE_OFFSET);
        s->loader_base = CXD4132_DDR_BASE + CXD4132_TEXT_OFFSET;
    } else {
        s->loader_base = cxd4132_init_loader(s);
    }

    qemu_register_reset(cxd4132_reset, s);
}

static void cxd4132_class_init(ObjectClass *klass, void *data)
{
    MachineClass *mc = MACHINE_CLASS(klass);

    mc->desc = "Sony BIONZ CXD4132";
    mc->init = cxd4132_init;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("arm11mpcore");
    mc->ignore_memory_transaction_failures = true;
}

static const TypeInfo cxd4132_info = {
    .name          = TYPE_CXD4132,
    .parent        = TYPE_MACHINE,
    .instance_size = sizeof(Cxd4132State),
    .class_init    = cxd4132_class_init,
};

static void cxd4132_register_type(void)
{
    type_register_static(&cxd4132_info);
}

type_init(cxd4132_register_type)
