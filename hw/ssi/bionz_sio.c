/* QEMU model of the Sony BIONZ synchronous serial peripheral (sio) */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "hw/ssi/ssi.h"
#include "hw/sysbus.h"

#define SIO_CS 0x00
#define SIO_SA 0x08
#define SIO_N  0x0c

#define SIO_CS_SIOST (1 << 5)
#define SIO_CS_ICL   (1 << 7)

#define TYPE_BIONZ_SIO "bionz_sio"
#define BIONZ_SIO(obj) OBJECT_CHECK(SioState, (obj), TYPE_BIONZ_SIO)

typedef struct SioState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    MemoryRegion bufram;

    qemu_irq intr;
    SSIBus *ssi;

    uint8_t reg_cs;
    uint8_t reg_sa;
    uint8_t reg_n;
} SioState;

static void sio_transfer(SioState *s)
{
    int i;
    uint8_t *buf = memory_region_get_ram_ptr(&s->bufram);
    for (i = 0; i <= s->reg_n; i++) {
        assert(s->reg_sa + i < 0x100);
        buf[s->reg_sa + i] = ssi_transfer(s->ssi, buf[s->reg_sa + i]);
    }
}

static uint64_t sio_read(void *opaque, hwaddr offset, unsigned size)
{
    SioState *s = BIONZ_SIO(opaque);
    uint32_t value;

    switch (offset) {
        case SIO_CS:
            value = s->reg_cs;
            break;

        case SIO_SA:
            value = s->reg_sa;
            break;

        case SIO_N:
            value = s->reg_n;
            break;

        default:
            qemu_log_mask(LOG_UNIMP, "%s: unimplemented read @ 0x%" HWADDR_PRIx "\n", __func__, offset);
            value = 0;
    }

    return value << 24;
}

static void sio_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    SioState *s = BIONZ_SIO(opaque);
    value >>= 24;

    switch (offset) {
        case SIO_CS:
            if (value & SIO_CS_ICL) {
                qemu_irq_lower(s->intr);
            }
            if (value & SIO_CS_SIOST) {
                sio_transfer(s);
                qemu_irq_raise(s->intr);
            }
            s->reg_cs = value & 0x1f;
            break;

        case SIO_SA:
            s->reg_sa = value;
            break;

        case SIO_N:
            s->reg_n = value;
            break;

        default:
            qemu_log_mask(LOG_UNIMP, "%s: unimplemented write @ 0x%" HWADDR_PRIx ": 0x%" PRIx64 "\n", __func__, offset, value);
    }
}

static const struct MemoryRegionOps sio_ops = {
    .read = sio_read,
    .write = sio_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void sio_reset(DeviceState *dev)
{
    SioState *s = BIONZ_SIO(dev);

    s->reg_cs = 0;
    s->reg_sa = 0;
    s->reg_n = 0;
}

static int sio_init(SysBusDevice *sbd)
{
    SioState *s = BIONZ_SIO(sbd);

    memory_region_init_io(&s->mmio, OBJECT(sbd), &sio_ops, s, TYPE_BIONZ_SIO ".mmio", 0x100);
    sysbus_init_mmio(sbd, &s->mmio);

    memory_region_init_ram_nomigrate(&s->bufram, OBJECT(sbd), TYPE_BIONZ_SIO ".buf", 0x100, &error_fatal);
    sysbus_init_mmio(sbd, &s->bufram);

    sysbus_init_irq(sbd, &s->intr);
    s->ssi = ssi_create_bus(DEVICE(sbd), "sio");

    return 0;
}

static void sio_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SysBusDeviceClass *k = SYS_BUS_DEVICE_CLASS(klass);

    k->init = sio_init;
    dc->reset = sio_reset;
}

static const TypeInfo sio_info = {
    .name          = TYPE_BIONZ_SIO,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SioState),
    .class_init    = sio_class_init,
};

static void sio_register_type(void)
{
    type_register_static(&sio_info);
}

type_init(sio_register_type)
