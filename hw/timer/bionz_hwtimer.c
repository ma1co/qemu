/* QEMU model of the Sony BIONZ timer block */

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "qemu/log.h"
#include "sysemu/sysemu.h"

#define TIMERCTL 0x00
#define TIMERCLR 0x04
#define TIMERCMP 0x08
#define TIMERREAD 0x0c
#define TIMERLOAD 0x10

#define CTL_RST(reg) ((reg) & 0x80000000)
#define CTL_EN(reg) ((reg) & 0x1000)
#define CTL_IEN(reg) ((reg) & 0x100)
#define CTL_MODE(reg) ((reg) & 0x30)
#define CTL_DIV(reg) ((reg) & 0x7)

#define CLR_CLR(reg) ((reg) & 0x10)
#define CLR_INTCLR(reg) ((reg) & 0x1)

#define MODE_PERIODIC 0x00
#define MODE_ONESHOT 0x10
#define MODE_FREERUN 0x30

#define TYPE_BIONZ_HWTIMER "bionz_hwtimer"
#define BIONZ_HWTIMER(obj) OBJECT_CHECK(HwtimerState, (obj), TYPE_BIONZ_HWTIMER)

typedef struct HwtimerState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    qemu_irq intr;

    uint32_t freq;

    QEMUTimer *timer;
    int64_t last_tick;
    int64_t next_tick;

    uint32_t reg_ctl;
    uint32_t reg_cmp;
    uint32_t reg_value;
} HwtimerState;

static uint32_t hwtimer_period(HwtimerState *s)
{
    return ((uint32_t) (1e9 / s->freq)) << CTL_DIV(s->reg_ctl);
}

static void hwtimer_reload(HwtimerState *s)
{
    if (CTL_EN(s->reg_ctl)) {
        s->next_tick = s->last_tick + (s->reg_cmp - s->reg_value) * hwtimer_period(s);
        timer_mod(s->timer, s->next_tick);
    } else {
        s->next_tick = 0;
        timer_del(s->timer);
    }
}

static void hwtimer_tick(void *opaque)
{
    HwtimerState *s = BIONZ_HWTIMER(opaque);

    s->reg_value += (s->next_tick - s->last_tick) / hwtimer_period(s) + 1;
    s->last_tick = s->next_tick;

    if (CTL_MODE(s->reg_ctl) == MODE_ONESHOT) {
        s->reg_ctl &= ~CTL_EN(s->reg_ctl);
    } else if (CTL_MODE(s->reg_ctl) == MODE_PERIODIC) {
        s->reg_value = 0;
    }
    hwtimer_reload(s);

    if (CTL_IEN(s->reg_ctl)) {
        qemu_irq_raise(s->intr);
    }
}

static void hwtimer_reset(DeviceState *dev)
{
    HwtimerState *s = BIONZ_HWTIMER(dev);

    s->last_tick = 0;
    s->next_tick = 0;
    s->reg_ctl = 0;
    s->reg_cmp = 0xffffffff;
    s->reg_value = 0;
    hwtimer_reload(s);
}

static uint64_t hwtimer_read(void *opaque, hwaddr offset, unsigned size)
{
    HwtimerState *s = BIONZ_HWTIMER(opaque);

    switch (offset) {
        case TIMERCTL:
            return s->reg_ctl;

        case TIMERCMP:
            return s->reg_cmp;

        case TIMERREAD:
            if (CTL_EN(s->reg_ctl)) {
                return s->reg_value + (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - s->last_tick) / hwtimer_period(s);
            } else {
                return s->reg_value;
            }

        default:
            qemu_log_mask(LOG_UNIMP, "%s: unimplemented read @ 0x%" HWADDR_PRIx "\n", __func__, offset);
            return 0;
    }
}

static void hwtimer_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    HwtimerState *s = BIONZ_HWTIMER(opaque);

    switch (offset) {
        case TIMERCTL:
            if (CTL_RST(value)) {
                hwtimer_reset(DEVICE(s));
            }
            if (!CTL_EN(s->reg_ctl)) {
                s->last_tick = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
            }
            s->reg_ctl = value & 0xffff;
            hwtimer_reload(s);
            break;

        case TIMERCLR:
            if (CLR_CLR(value)) {
                s->reg_value = 0;
                s->last_tick = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
                hwtimer_reload(s);
            }
            if (CLR_INTCLR(value)) {
                qemu_irq_lower(s->intr);
            }
            break;

        case TIMERCMP:
            s->reg_cmp = value;
            hwtimer_reload(s);
            break;

        case TIMERLOAD:
            s->reg_value = value;
            s->last_tick = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
            hwtimer_reload(s);
            break;

        default:
            qemu_log_mask(LOG_UNIMP, "%s: unimplemented write @ 0x%" HWADDR_PRIx ": 0x%" PRIx64 "\n", __func__, offset, value);
    }
}

static const struct MemoryRegionOps hwtimer_ops = {
    .read = hwtimer_read,
    .write = hwtimer_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void hwtimer_realize(DeviceState *dev, Error **errp)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    HwtimerState *s = BIONZ_HWTIMER(dev);

    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, hwtimer_tick, s);

    memory_region_init_io(&s->mmio, OBJECT(dev), &hwtimer_ops, s, TYPE_BIONZ_HWTIMER, 0x20);
    sysbus_init_mmio(sbd, &s->mmio);
    sysbus_init_irq(sbd, &s->intr);
}

static Property hwtimer_properties[] = {
    DEFINE_PROP_UINT32("freq", HwtimerState, freq, 4e6),
    DEFINE_PROP_END_OF_LIST()
};

static void hwtimer_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = hwtimer_realize;
    dc->reset = hwtimer_reset;
    dc->props = hwtimer_properties;
}

static const TypeInfo hwtimer_info = {
    .name          = TYPE_BIONZ_HWTIMER,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(HwtimerState),
    .class_init    = hwtimer_class_init,
};

static void hwtimer_register_type(void)
{
    type_register_static(&hwtimer_info);
}

type_init(hwtimer_register_type)
