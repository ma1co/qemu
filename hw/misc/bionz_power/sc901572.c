/* QEMU model of the Sony SC901572VOR power IC */

#include "qemu/osdep.h"
#include "hw/hw.h"
#include "hw/ssi/ssi.h"
#include "checksum.h"
#include "qemu/timer.h"

#define TYPE_BIONZ_SC901572 "bionz_sc901572"
#define BIONZ_SC901572(obj) OBJECT_CHECK(Sc901572State, (obj), TYPE_BIONZ_SC901572)

typedef struct Sc901572State {
    SSISlave parent_obj;
    uint8_t inbuf[259];
    uint8_t outbuf[259];
    uint16_t buf_pos;

    int64_t time;
    bool time_valid;
} Sc901572State;

static void sc901572_start_transfer(Sc901572State *s, uint8_t type)
{
    if (type & 1) {
        stl_le_p(&s->outbuf[2], s->time + get_clock_realtime() / NANOSECONDS_PER_SECOND);
        s->outbuf[8] = parity(s->outbuf + 1, 7, 1);
    } else if (type & 2) {
        s->outbuf[258] = parity(s->outbuf + 1, 257, 1);
    } else {
        s->outbuf[14] = parity(s->outbuf + 1, 13, 1);

        // hack to also support MB89083LGA:
        s->outbuf[126] = parity(s->outbuf, 126, 2) ^ 0x0f;
        s->outbuf[127] = parity(s->outbuf + 1, 126, 2) ^ 0x0f;
    }
}

static void sc901572_end_transfer(Sc901572State *s)
{
    if (s->inbuf[0] == 0xe1 && s->inbuf[1] == 5) {
        s->time = ldl_le_p(&s->inbuf[2]);
        s->time_valid = s->time != 0;
        s->time -= get_clock_realtime() / NANOSECONDS_PER_SECOND;
    }

    memset(s->outbuf, 0, sizeof(s->outbuf));
    s->outbuf[1] = s->time_valid ? 0x10 : 0;
}

static uint32_t sc901572_transfer(SSISlave *dev, uint32_t value)
{
    Sc901572State *s = BIONZ_SC901572(dev);
    if (s->buf_pos >= sizeof(s->inbuf)) {
        hw_error("%s: overflow", __func__);
    }
    uint8_t ret = s->outbuf[s->buf_pos];
    s->inbuf[s->buf_pos] = value;
    if (s->buf_pos == 1) {
        sc901572_start_transfer(s, value);
    }
    s->buf_pos++;
    return ret;
}

static int sc901572_set_cs(SSISlave *dev, bool cs)
{
    Sc901572State *s = BIONZ_SC901572(dev);
    if (cs) {
        sc901572_end_transfer(s);
        s->buf_pos = 0;
    }
    return 0;
}

static void sc901572_realize(SSISlave *dev, Error **errp)
{
    Sc901572State *s = BIONZ_SC901572(dev);

    memset(s->inbuf, 0, sizeof(s->inbuf));
    sc901572_end_transfer(s);
    s->buf_pos = 0;
    s->time = 0;
    s->time_valid = 0;
}

static void sc901572_class_init(ObjectClass *klass, void *data)
{
    SSISlaveClass *k = SSI_SLAVE_CLASS(klass);

    k->realize = sc901572_realize;
    k->transfer = sc901572_transfer;
    k->set_cs = sc901572_set_cs;
    k->cs_polarity = SSI_CS_LOW;
}

static const TypeInfo sc901572_info = {
    .name          = TYPE_BIONZ_SC901572,
    .parent        = TYPE_SSI_SLAVE,
    .instance_size = sizeof(Sc901572State),
    .class_init    = sc901572_class_init,
};

static void sc901572_register_type(void)
{
    type_register_static(&sc901572_info);
}

type_init(sc901572_register_type)
