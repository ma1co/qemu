/* Bus model to connect analog devices to ADCs */

#include "qemu/osdep.h"
#include "analog.h"

struct AnalogBus {
    BusState parent_obj;
    analog_set_func set_func;
    void *set_opaque;
};

AnalogBus *analog_bus_create(DeviceState *parent, const char *name, analog_set_func set_func, void *set_opaque)
{
    AnalogBus *bus = ANALOG_BUS(qbus_create(TYPE_ANALOG_BUS, parent, name));
    bus->set_func = set_func;
    bus->set_opaque = set_opaque;
    return bus;
}

void analog_bus_set(AnalogBus *bus, unsigned int channel, unsigned int value, unsigned int max)
{
    if (bus->set_func) {
        bus->set_func(bus->set_opaque, channel, value, max);
    }
}

static const TypeInfo analog_bus_info = {
    .name = TYPE_ANALOG_BUS,
    .parent = TYPE_BUS,
    .instance_size = sizeof(AnalogBus),
};

static void analog_bus_register_type(void)
{
    type_register_static(&analog_bus_info);
}

type_init(analog_bus_register_type)
