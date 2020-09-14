/* QEMU model of the Sony BIONZ nand coprocessor (boss) */

#include "qemu/osdep.h"
#include "exec/address-spaces.h"
#include "hw/irq.h"
#include "hw/sysbus.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "sysemu/cpus.h"
#include "target/arm/arm-powerctl.h"
#include "target/arm/cpu.h"

#define BOSS_CPUID 0xB055
#define BOSS_SRAM_BASE 0x00000000
#define BOSS_INTC_BASE 0xfffff000

#define BOSS_IRQ_MAIN2BOSS (1 << 0)
#define BOSS_IRQ_NAND      (1 << 2)

#define TYPE_BIONZ_BOSS "bionz_boss"
#define BIONZ_BOSS(obj) OBJECT_CHECK(BossState, (obj), TYPE_BIONZ_BOSS)

#define TYPE_BIONZ_BOSS_CPU "bionz_boss_cpu"
#define BIONZ_BOSS_CPU_CLASS(klass) OBJECT_CLASS_CHECK(BossCPUClass, (klass), TYPE_BIONZ_BOSS_CPU)
#define BIONZ_BOSS_CPU_GET_CLASS(obj) OBJECT_GET_CLASS(BossCPUClass, (obj), TYPE_BIONZ_BOSS_CPU)

typedef struct BossState {
    SysBusDevice parent_obj;
    ARMCPU cpu;
    MemoryRegion container;
    MemoryRegion system_memory_alias;
    MemoryRegion sram;
    MemoryRegion io;
    MemoryRegion clkrst;
    MemoryRegion intc;
    qemu_irq irq_ext;

    uint32_t enable;
    uint32_t irq_int_status;
    uint32_t irq_ext_status;
} BossState;

typedef struct BossCPUClass {
    ARMCPUClass parent_class;
    void (*parent_reset)(DeviceState *dev);
} BossCPUClass;

static void boss_update_irq(BossState *s)
{
    qemu_set_irq(qdev_get_gpio_in(DEVICE(&s->cpu), ARM_CPU_IRQ), s->irq_int_status);
    qemu_set_irq(s->irq_ext, s->irq_ext_status);
}

static uint64_t boss_io_read(void *opaque, hwaddr offset, unsigned size)
{
    BossState *s = BIONZ_BOSS(opaque);

    switch (offset) {
        case 0x00:
            return s->irq_ext_status;

        case 0x04:
            return !!(s->irq_int_status & BOSS_IRQ_MAIN2BOSS);

        default:
            qemu_log_mask(LOG_UNIMP, "%s: unimplemented read @ 0x%" HWADDR_PRIx "\n", __func__, offset);
            return 0;
    }
}

static void boss_io_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    BossState *s = BIONZ_BOSS(opaque);

    switch (offset) {
        case 0x00:
            s->irq_ext_status = value & 1;
            boss_update_irq(s);
            if ((value & 1) && current_cpu == CPU(&s->cpu)) {
                cpu_stop_current();
            }
            break;

        case 0x04:
            if (value & 1) {
                s->irq_int_status |= BOSS_IRQ_MAIN2BOSS;
            } else {
                s->irq_int_status &= ~BOSS_IRQ_MAIN2BOSS;
            }
            boss_update_irq(s);
            if (value & 1) {
                cpu_resume(CPU(&s->cpu));
            }
            break;

        default:
            qemu_log_mask(LOG_UNIMP, "%s: unimplemented write @ 0x%" HWADDR_PRIx ": 0x%" PRIx64 "\n", __func__, offset, value);
    }
}

static const struct MemoryRegionOps boss_io_ops = {
    .read = boss_io_read,
    .write = boss_io_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void boss_update_power(BossState *s)
{
    if (s->enable) {
        arm_set_cpu_on(BOSS_CPUID, BOSS_SRAM_BASE, 0, arm_highest_el(&s->cpu.env), false);
    } else {
        arm_set_cpu_off(BOSS_CPUID);
    }
}

static uint64_t boss_clkrst_read(void *opaque, hwaddr offset, unsigned size)
{
    BossState *s = BIONZ_BOSS(opaque);

    switch (offset) {
        case 0x00:
            return s->enable;

        default:
            qemu_log_mask(LOG_UNIMP, "%s: unimplemented read @ 0x%" HWADDR_PRIx "\n", __func__, offset);
            return 0;
    }
}

static void boss_clkrst_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    BossState *s = BIONZ_BOSS(opaque);

    switch (offset) {
        case 0x00:
            s->enable = value;
            boss_update_power(s);
            break;

        case 0x04:
            s->enable |= value;
            boss_update_power(s);
            break;

        case 0x08:
            s->enable &= ~value;
            boss_update_power(s);
            break;

        default:
            qemu_log_mask(LOG_UNIMP, "%s: unimplemented write @ 0x%" HWADDR_PRIx ": 0x%" PRIx64 "\n", __func__, offset, value);
    }
}

static const struct MemoryRegionOps boss_clkrst_ops = {
    .read = boss_clkrst_read,
    .write = boss_clkrst_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static uint64_t boss_intc_read(void *opaque, hwaddr offset, unsigned size)
{
    BossState *s = BIONZ_BOSS(opaque);
    uint32_t status, off;

    switch (offset) {
        case 0xe20:
            if (s->irq_int_status) {
                status = s->irq_int_status;
                off = 0x20;
                while (!(status & 1)) {
                    status >>= 1;
                    off += 4;
                }
                return off;
            }
            break;
    }

    qemu_log_mask(LOG_UNIMP, "%s: unimplemented read @ 0x%" HWADDR_PRIx "\n", __func__, offset);
    return 0;
}

static void boss_intc_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    qemu_log_mask(LOG_UNIMP, "%s: unimplemented write @ 0x%" HWADDR_PRIx ": 0x%" PRIx64 "\n", __func__, offset, value);
}

static const struct MemoryRegionOps boss_intc_ops = {
    .read = boss_intc_read,
    .write = boss_intc_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void boss_reset(DeviceState *dev)
{
    BossState *s = BIONZ_BOSS(dev);

    s->enable = 0;
    s->irq_int_status = 0;
    s->irq_ext_status = 0;
}

static void boss_irq_nand_handler(void *opaque, int irq, int level)
{
    BossState *s = BIONZ_BOSS(opaque);
    if (level) {
        s->irq_int_status |= BOSS_IRQ_NAND;
    } else {
        s->irq_int_status &= ~BOSS_IRQ_NAND;
    }
    boss_update_irq(s);
}

static void boss_realize(DeviceState *dev, Error **errp)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    BossState *s = BIONZ_BOSS(dev);

    memory_region_init(&s->container, OBJECT(dev), TYPE_BIONZ_BOSS ".container", UINT64_MAX);
    memory_region_init_alias(&s->system_memory_alias, OBJECT(dev), TYPE_BIONZ_BOSS ".sysmem", get_system_memory(), 0, UINT64_MAX);
    memory_region_add_subregion(&s->container, 0, &s->system_memory_alias);

    object_initialize(&s->cpu, sizeof(s->cpu), TYPE_BIONZ_BOSS_CPU);
    object_property_set_bool(OBJECT(&s->cpu), false, "has_el3", &error_fatal);
    object_property_set_int(OBJECT(&s->cpu), BOSS_CPUID, "mp-affinity", &error_fatal);
    object_property_set_bool(OBJECT(&s->cpu), true, "start-powered-off", &error_fatal);
    object_property_set_link(OBJECT(&s->cpu), OBJECT(&s->container), "memory", &error_fatal);
    qdev_init_nofail(DEVICE(&s->cpu));

    memory_region_init_ram(&s->sram, OBJECT(dev), TYPE_BIONZ_BOSS ".sram", 0x4000, &error_fatal);
    sysbus_init_mmio(sbd, &s->sram);

    memory_region_init_io(&s->io, OBJECT(dev), &boss_io_ops, s, TYPE_BIONZ_BOSS ".io", 0x10);
    sysbus_init_mmio(sbd, &s->io);

    memory_region_init_io(&s->clkrst, OBJECT(dev), &boss_clkrst_ops, s, TYPE_BIONZ_BOSS ".clkrst", 0x10);
    sysbus_init_mmio(sbd, &s->clkrst);

    memory_region_init_io(&s->intc, OBJECT(dev), &boss_intc_ops, s, TYPE_BIONZ_BOSS ".intc", 0x1000);
    memory_region_add_subregion(&s->container, BOSS_INTC_BASE, &s->intc);

    qdev_init_gpio_in(dev, boss_irq_nand_handler, 1);
    sysbus_init_irq(sbd, &s->irq_ext);
}

static void boss_cpu_reset(DeviceState *dev)
{
    CPUState *s = CPU(dev);
    BossCPUClass *bcc = BIONZ_BOSS_CPU_GET_CLASS(dev);

    uint32_t ir = s->interrupt_request;
    bcc->parent_reset(dev);
    s->interrupt_request = ir;
}

static void boss_cpu_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    BossCPUClass *bcc = BIONZ_BOSS_CPU_CLASS(klass);
    device_class_set_parent_reset(dc, boss_cpu_reset, &bcc->parent_reset);
}

static const TypeInfo boss_cpu_info = {
    .name          = TYPE_BIONZ_BOSS_CPU,
    .parent        = ARM_CPU_TYPE_NAME("cortex-a9"),// not sure
    .class_size    = sizeof(BossCPUClass),
    .class_init    = boss_cpu_class_init,
};

static void boss_cpu_register_type(void)
{
    type_register_static(&boss_cpu_info);
}

type_init(boss_cpu_register_type)

static void boss_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = boss_realize;
    dc->reset = boss_reset;
}

static const TypeInfo boss_info = {
    .name          = TYPE_BIONZ_BOSS,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(BossState),
    .class_init    = boss_class_init,
};

static void boss_register_type(void)
{
    type_register_static(&boss_info);
}

type_init(boss_register_type)
