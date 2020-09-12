/* QEMU model of the Sony CA power IC (19A44FDAXBG) */

#include "qemu/osdep.h"
#include "hw/irq.h"
#include "hw/ssi/ssi.h"
#include "qemu/timer.h"

#define TYPE_BIONZ_CA "bionz_ca"
#define BIONZ_CA(obj) OBJECT_CHECK(CaState, (obj), TYPE_BIONZ_CA)

typedef struct CaState {
    SSISlave parent_obj;
    qemu_irq req;
    QEMUTimer *reset_timer;

    uint8_t buf[8];
    uint8_t buf_pos;
} CaState;

static void ca_cmd(CaState *s)
{
    memset(s->buf, 0, sizeof(s->buf));
    s->buf[0] = 1;
    s->buf[2] = sizeof(s->buf);

    qemu_irq_lower(s->req);
    qemu_irq_raise(s->req);
}

static uint32_t ca_transfer(SSISlave *dev, uint32_t value)
{
    CaState *s = BIONZ_CA(dev);
    uint8_t ret = s->buf[s->buf_pos];
    s->buf[s->buf_pos] = value;
    s->buf_pos++;
    if (s->buf_pos >= sizeof(s->buf)) {
        ca_cmd(s);
        s->buf_pos = 0;
    }
    return ret;
}

static void ca_reset(void *opaque)
{
    CaState *s = BIONZ_CA(opaque);
    ca_cmd(s);
    s->buf_pos = 0;
}

static void ca_realize(SSISlave *dev, Error **errp)
{
    CaState *s = BIONZ_CA(dev);

    qdev_init_gpio_out_named(DEVICE(dev), &s->req, "req", 1);

    s->reset_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, ca_reset, s);
    timer_mod(s->reset_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
}

static void ca_class_init(ObjectClass *klass, void *data)
{
    SSISlaveClass *k = SSI_SLAVE_CLASS(klass);

    k->realize = ca_realize;
    k->transfer = ca_transfer;
    k->cs_polarity = SSI_CS_NONE;
}

static const TypeInfo ca_info = {
    .name          = TYPE_BIONZ_CA,
    .parent        = TYPE_SSI_SLAVE,
    .instance_size = sizeof(CaState),
    .class_init    = ca_class_init,
};

static void ca_register_type(void)
{
    type_register_static(&ca_info);
}

type_init(ca_register_type)
