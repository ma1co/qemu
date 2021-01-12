/* A constant analog voltage that can be wired to an ADC */

#include "qemu/osdep.h"
#include "analog.h"
#include "hw/qdev-properties.h"
#include "sysemu/sysemu.h"

#define TYPE_ANALOG_VOLTAGE "analog_voltage"
#define ANALOG_VOLTAGE(obj) OBJECT_CHECK(AnalogVoltageState, (obj), TYPE_ANALOG_VOLTAGE)

typedef struct AnalogVoltageState {
    DeviceState parent_obj;
    QEMUTimer *timer;

    uint8_t channel;
    uint8_t value;
    uint8_t max;
} AnalogVoltageState;

static void analog_voltage_update(void *opaque)
{
    AnalogVoltageState *s = ANALOG_VOLTAGE(opaque);
    AnalogBus *bus = ANALOG_BUS(qdev_get_parent_bus(DEVICE(s)));
    analog_bus_set(bus, s->channel, s->value, s->max);
}

static void analog_voltage_reset(DeviceState *dev)
{
    AnalogVoltageState *s = ANALOG_VOLTAGE(dev);
    timer_del(s->timer);
    timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
}

static void analog_voltage_realize(DeviceState *dev, Error **errp)
{
    AnalogVoltageState *s = ANALOG_VOLTAGE(dev);
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, analog_voltage_update, s);
}

static Property analog_voltage_properties[] = {
    DEFINE_PROP_UINT8("channel", AnalogVoltageState, channel, 0),
    DEFINE_PROP_UINT8("value", AnalogVoltageState, value, 0),
    DEFINE_PROP_UINT8("max", AnalogVoltageState, max, 255),
    DEFINE_PROP_END_OF_LIST(),
};

static void analog_voltage_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->bus_type = TYPE_ANALOG_BUS;
    dc->realize = analog_voltage_realize;
    device_class_set_props(dc, analog_voltage_properties);
    dc->reset = analog_voltage_reset;
}

static const TypeInfo analog_voltage_info = {
    .name          = TYPE_ANALOG_VOLTAGE,
    .parent        = TYPE_DEVICE,
    .instance_size = sizeof(AnalogVoltageState),
    .class_init    = analog_voltage_class_init,
};

static void analog_voltage_register_type(void)
{
    type_register_static(&analog_voltage_info);
}

type_init(analog_voltage_register_type)
