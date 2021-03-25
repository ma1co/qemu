/* QEMU model of the Sony CXD4108 sysv tick timer */

#include "qemu/osdep.h"
#include "hw/irq.h"
#include "hw/sysbus.h"
#include "qemu/log.h"
#include "sysemu/sysemu.h"

#define NUM_IRQ 10
#define PERIOD_NS 16683333 // NTSC

#define TYPE_BIONZ_SYSV "bionz_sysv"
#define BIONZ_SYSV(obj) OBJECT_CHECK(SysvState, (obj), TYPE_BIONZ_SYSV)

typedef struct SysvState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    qemu_irq irqs[NUM_IRQ];

    QEMUTimer *timer;

    uint32_t reg_en0;
    uint32_t reg_en1;
    uint32_t reg_intsts;
} SysvState;

static void sysv_set_timer(SysvState *s)
{
    timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + PERIOD_NS);
}

static void sysv_update(SysvState *s)
{
    int i;
    for (i = 0; i < NUM_IRQ; i++) {
        qemu_set_irq(s->irqs[i], (s->reg_intsts >> i) & 1);
    }
}

static void sysv_tick(void *opaque)
{
    SysvState *s = BIONZ_SYSV(opaque);

    s->reg_intsts |= s->reg_en0 & s->reg_en1 & 0b1001001;
    sysv_update(s);
    sysv_set_timer(s);
}

static uint64_t sysv_read(void *opaque, hwaddr offset, unsigned size)
{
    SysvState *s = BIONZ_SYSV(opaque);

    switch (offset) {
        case 0x18:
            return s->reg_en0;

        case 0x1c:
            return s->reg_en1;

        default:
            qemu_log_mask(LOG_UNIMP, "%s: unimplemented read @ 0x%" HWADDR_PRIx "\n", __func__, offset);
            return 0;
    }
}

static void sysv_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    SysvState *s = BIONZ_SYSV(opaque);

    switch (offset) {
        case 0x18:
            s->reg_en0 = value;
            break;

        case 0x1c:
            s->reg_en1 = value;
            break;

        case 0x24:
            s->reg_intsts &= ~value;
            sysv_update(s);
            break;

        default:
            qemu_log_mask(LOG_UNIMP, "%s: unimplemented write @ 0x%" HWADDR_PRIx ": 0x%" PRIx64 "\n", __func__, offset, value);
    }
}

static const struct MemoryRegionOps sysv_ops = {
    .read = sysv_read,
    .write = sysv_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void sysv_reset(DeviceState *dev)
{
    SysvState *s = BIONZ_SYSV(dev);

    s->reg_en0 = 0;
    s->reg_en1 = 0;
    s->reg_intsts = 0;

    timer_del(s->timer);
    sysv_set_timer(s);
    sysv_update(s);
}

static void sysv_realize(DeviceState *dev, Error **errp)
{
    int i;
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    SysvState *s = BIONZ_SYSV(dev);

    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, sysv_tick, s);

    memory_region_init_io(&s->mmio, OBJECT(dev), &sysv_ops, s, TYPE_BIONZ_SYSV, 0x30);
    sysbus_init_mmio(sbd, &s->mmio);
    for (i = 0; i < NUM_IRQ; i++) {
        sysbus_init_irq(sbd, &s->irqs[i]);
    }
}

static void sysv_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = sysv_realize;
    dc->reset = sysv_reset;
}

static const TypeInfo sysv_info = {
    .name          = TYPE_BIONZ_SYSV,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SysvState),
    .class_init    = sysv_class_init,
};

static void sysv_register_type(void)
{
    type_register_static(&sysv_info);
}

type_init(sysv_register_type)
