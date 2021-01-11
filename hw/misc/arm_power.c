/* ARM CPU power switch */

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "hw/qdev-properties.h"
#include "target/arm/arm-powerctl.h"

#define TYPE_ARM_POWER "arm_power"
#define ARM_POWER(obj) OBJECT_CHECK(ArmPowerState, (obj), TYPE_ARM_POWER)

typedef struct ArmPowerState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;

    uint8_t mask;
    uint64_t cpuid;

    uint8_t value;
} ArmPowerState;

static uint64_t arm_power_read(void *opaque, hwaddr offset, unsigned size)
{
    ArmPowerState *s = ARM_POWER(opaque);
    return s->value;
}

static void arm_power_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    ArmPowerState *s = ARM_POWER(opaque);
    if ((s->value ^ value) & s->mask) {
        if (value & s->mask) {
            arm_set_cpu_on_and_reset(s->cpuid);
        } else {
            arm_set_cpu_off(s->cpuid);
        }
    }
    s->value = value;
}

static const struct MemoryRegionOps arm_power_ops = {
    .read = arm_power_read,
    .write = arm_power_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void arm_power_reset(DeviceState *dev)
{
    ArmPowerState *s = ARM_POWER(dev);
    s->value = 0;
}

static void arm_power_realize(DeviceState *dev, Error **errp)
{
    ArmPowerState *s = ARM_POWER(dev);
    memory_region_init_io(&s->mmio, OBJECT(dev), &arm_power_ops, s, TYPE_ARM_POWER, 1);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
}

static Property arm_power_properties[] = {
    DEFINE_PROP_UINT8("mask", ArmPowerState, mask, 1),
    DEFINE_PROP_UINT64("cpuid", ArmPowerState, cpuid, 0),
    DEFINE_PROP_END_OF_LIST(),
};

static void arm_power_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = arm_power_realize;
    device_class_set_props(dc, arm_power_properties);
    dc->reset = arm_power_reset;
}

static const TypeInfo arm_power_info = {
    .name          = TYPE_ARM_POWER,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ArmPowerState),
    .class_init    = arm_power_class_init,
};

static void arm_power_register_type(void)
{
    type_register_static(&arm_power_info);
}

type_init(arm_power_register_type)
