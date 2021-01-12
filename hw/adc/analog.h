#ifndef HW_ANALOG_H
#define HW_ANALOG_H

#include "hw/qdev-core.h"

#define TYPE_ANALOG_BUS "analog_bus"
#define ANALOG_BUS(obj) OBJECT_CHECK(AnalogBus, (obj), TYPE_ANALOG_BUS)

typedef void (*analog_set_func)(void *opaque, unsigned int channel, unsigned int value, unsigned int max);

typedef struct AnalogBus AnalogBus;

AnalogBus *analog_bus_create(DeviceState *parent, const char *name, analog_set_func set_func, void *set_opaque);

void analog_bus_set(AnalogBus *bus, unsigned int channel, unsigned int value, unsigned int max);

#endif
