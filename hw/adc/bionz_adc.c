/* QEMU model of the Sony CXD4108 ADC */

#include "qemu/osdep.h"
#include "analog.h"
#include "hw/irq.h"
#include "hw/sysbus.h"
#include "qemu/log.h"

#define NUM_CHANNELS 8
#define MAX_VALUE 0x3ff

#define TYPE_BIONZ_ADC "bionz_adc"
#define BIONZ_ADC(obj) OBJECT_CHECK(AdcState, (obj), TYPE_BIONZ_ADC)

typedef struct AdcState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;

    qemu_irq irq;
    AnalogBus *analog;

    uint16_t ctrl;
    uint16_t inputs[NUM_CHANNELS];
    uint16_t sampled[NUM_CHANNELS];
} AdcState;

static void adc_set(void *opaque, unsigned int channel, unsigned int value, unsigned int max)
{
    AdcState *s = BIONZ_ADC(opaque);
    assert(channel < NUM_CHANNELS);
    assert(max > 0);
    s->inputs[channel] = value * MAX_VALUE / max;
}

static uint64_t adc_read(void *opaque, hwaddr offset, unsigned size)
{
    AdcState *s = BIONZ_ADC(opaque);

    if (offset >= 8 && offset < 8 + (NUM_CHANNELS << 2) && !(offset & 3)) {
        return s->sampled[(offset - 8) >> 2];
    } else {
        switch (offset) {
            case 0x04:
                return s->ctrl;

            default:
                qemu_log_mask(LOG_UNIMP, "%s: unimplemented read @ 0x%" HWADDR_PRIx "\n", __func__, offset);
                return 0;
        }
    }
}

static void adc_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    AdcState *s = BIONZ_ADC(opaque);
    int i;

    switch (offset) {
        case 0x04:
            if (value & 1) {
                for (i = 0; i < NUM_CHANNELS; i++) {
                    s->sampled[i] = s->inputs[i];
                }
                s->ctrl |= 4;
            }
            if (value & 8) {
                s->ctrl &= ~4;
            }
            qemu_set_irq(s->irq, !!(s->ctrl & 4));
            break;

        default:
            qemu_log_mask(LOG_UNIMP, "%s: unimplemented write @ 0x%" HWADDR_PRIx ": 0x%" PRIx64 "\n", __func__, offset, value);
    }
}

static const struct MemoryRegionOps adc_ops = {
    .read = adc_read,
    .write = adc_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 2,
    .valid.max_access_size = 4,
};

static void adc_reset(DeviceState *dev)
{
    AdcState *s = BIONZ_ADC(dev);
    int i;

    s->ctrl = 0;
    for (i = 0; i < NUM_CHANNELS; i++) {
        s->inputs[i] = MAX_VALUE;
        s->sampled[i] = MAX_VALUE;
    }
}

static void adc_realize(DeviceState *dev, Error **errp)
{
    AdcState *s = BIONZ_ADC(dev);

    memory_region_init_io(&s->mmio, OBJECT(dev), &adc_ops, s, TYPE_BIONZ_ADC, 0x100);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);

    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
    s->analog = analog_bus_create(dev, "analog", adc_set, s);
}

static void adc_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = adc_realize;
    dc->reset = adc_reset;
}

static const TypeInfo adc_info = {
    .name          = TYPE_BIONZ_ADC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(AdcState),
    .class_init    = adc_class_init,
};

static void adc_register_type(void)
{
    type_register_static(&adc_info);
}

type_init(adc_register_type)
