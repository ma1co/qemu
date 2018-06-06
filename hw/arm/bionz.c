/* QEMU model of the Sony BIONZ CXD4132 image processor */

#include "qemu/osdep.h"
#include "cpu.h"
#include "hw/boards.h"
#include "hw/loader.h"
#include "hw/sysbus.h"
#include "hw/char/pl011.h"
#include "hw/cpu/arm11mpcore.h"
#include "qapi/error.h"
#include "sysemu/sysemu.h"

#define CXD4132_DDR_BASE 0x80000000
#define CXD4132_DDR_SIZE 0x20000000

#define CXD4132_SRAM_BASE 0xa0000000
#define CXD4132_SRAM_SIZE 0x00400000

#define CXD4132_HWTIMER_BASE(i) (0xf2008000 + (i) * 0x20)
#define CXD4132_NUM_HWTIMER 5

#define CXD4132_UART_BASE(i) (0xf2038000 + (i) * 0x1000)
#define CXD4132_NUM_UART 3

#define CXD4132_MISCCTRL_BASE 0xf3060000

#define CXD4132_MPCORE_BASE 0xf8000000

#define CXD4132_IRQ_UART(i) (160 + (i))
#define CXD4132_IRQ_HWTIMER(i) (163 + (i))

#define CXD4132_NUM_IRQ 256
#define CXD4132_IRQ_OFFSET 32

#define CXD4132_CMDLINE_OFFSET 0x00013000
#define CXD4132_TEXT_OFFSET 0x00018000
#define CXD4132_INITRD_OFFSET 0x00408000

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
    hwaddr loader_base;
} Cxd4132State;

static void cxd4132_reset(void *opaque)
{
    Cxd4132State *s = CXD4132(opaque);
    cpu_reset(CPU(&s->cpu));
    cpu_set_pc(CPU(&s->cpu), s->loader_base);
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
    MemoryRegion *mem;
    DeviceState *dev;
    qemu_irq irq[CXD4132_NUM_IRQ - CXD4132_IRQ_OFFSET];
    int i;

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

    load_image_targphys(machine->kernel_filename, CXD4132_DDR_BASE + CXD4132_TEXT_OFFSET, CXD4132_DDR_SIZE - CXD4132_TEXT_OFFSET);
    load_image_targphys(machine->initrd_filename, CXD4132_DDR_BASE + CXD4132_INITRD_OFFSET, CXD4132_DDR_SIZE - CXD4132_INITRD_OFFSET);
    cxd4132_init_cmdline(CXD4132_CMDLINE, machine->kernel_cmdline, CXD4132_DDR_BASE + CXD4132_CMDLINE_OFFSET);

    s->loader_base = CXD4132_DDR_BASE + CXD4132_TEXT_OFFSET;
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
