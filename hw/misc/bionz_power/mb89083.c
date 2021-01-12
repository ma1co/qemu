/* QEMU model of the Sony MB89083LGA power IC */

#include "qemu/osdep.h"
#include "hw/ssi/ssi.h"
#include "checksum.h"

#define TYPE_BIONZ_MB89083 "bionz_mb89083"
#define BIONZ_MB89083(obj) OBJECT_CHECK(Mb89083State, (obj), TYPE_BIONZ_MB89083)

typedef struct Mb89083State {
    SSISlave parent_obj;
    uint8_t buf[128];
    uint8_t buf_pos;
} Mb89083State;

static void mb89083_cmd(Mb89083State *s)
{
    memset(s->buf, 0, sizeof(s->buf));
    s->buf[14] = parity(s->buf + 1, 13, 1); // hack to also support SC901572VOR
    s->buf[126] = parity(s->buf, 126, 2) ^ 0x0f;
    s->buf[127] = parity(s->buf + 1, 126, 2) ^ 0x0f;
}

static uint32_t mb89083_transfer(SSISlave *dev, uint32_t value)
{
    Mb89083State *s = BIONZ_MB89083(dev);
    uint8_t ret = s->buf[s->buf_pos];
    s->buf[s->buf_pos] = value;
    s->buf_pos++;
    if (s->buf_pos >= sizeof(s->buf)) {
        mb89083_cmd(s);
        s->buf_pos = 0;
    }
    return ret;
}

static int mb89083_set_cs(SSISlave *dev, bool cs)
{
    Mb89083State *s = BIONZ_MB89083(dev);
    if (cs) {
        // hack to also support SC901572VOR
        mb89083_cmd(s);
        s->buf_pos = 0;
    }
    return 0;
}

static void mb89083_realize(SSISlave *dev, Error **errp)
{
    Mb89083State *s = BIONZ_MB89083(dev);

    mb89083_cmd(s);
    s->buf_pos = 0;
}

static void mb89083_class_init(ObjectClass *klass, void *data)
{
    SSISlaveClass *k = SSI_SLAVE_CLASS(klass);

    k->realize = mb89083_realize;
    k->transfer = mb89083_transfer;
    k->set_cs = mb89083_set_cs;
    k->cs_polarity = SSI_CS_LOW;
}

static const TypeInfo mb89083_info = {
    .name          = TYPE_BIONZ_MB89083,
    .parent        = TYPE_SSI_SLAVE,
    .instance_size = sizeof(Mb89083State),
    .class_init    = mb89083_class_init,
};

static void mb89083_register_type(void)
{
    type_register_static(&mb89083_info);
}

type_init(mb89083_register_type)
