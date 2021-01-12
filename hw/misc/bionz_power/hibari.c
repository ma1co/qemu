/* QEMU model of the Sony Hibari power IC (MB44C031PW) */

#include "qemu/osdep.h"
#include "hw/ssi/ssi.h"
#include "checksum.h"

#define TYPE_BIONZ_HIBARI "bionz_hibari"
#define BIONZ_HIBARI(obj) OBJECT_CHECK(HibariState, (obj), TYPE_BIONZ_HIBARI)

typedef struct HibariState {
    SSISlave parent_obj;
    uint8_t buf[10];
    uint8_t buf_pos;
} HibariState;

static void hibari_cmd(HibariState *s)
{
    memset(s->buf, 0, sizeof(s->buf));
    s->buf[9] = parity(s->buf, 9, 1) ^ 0x0f;
}

static uint32_t hibari_transfer(SSISlave *dev, uint32_t value)
{
    HibariState *s = BIONZ_HIBARI(dev);
    uint8_t ret = s->buf[s->buf_pos];
    s->buf[s->buf_pos] = value;
    s->buf_pos++;
    if (s->buf_pos >= sizeof(s->buf)) {
        hibari_cmd(s);
        s->buf_pos = 0;
    }
    return ret;
}

static void hibari_realize(SSISlave *dev, Error **errp)
{
    HibariState *s = BIONZ_HIBARI(dev);

    hibari_cmd(s);
    s->buf_pos = 0;
}

static void hibari_class_init(ObjectClass *klass, void *data)
{
    SSISlaveClass *k = SSI_SLAVE_CLASS(klass);

    k->realize = hibari_realize;
    k->transfer = hibari_transfer;
    k->cs_polarity = SSI_CS_LOW;
}

static const TypeInfo hibari_info = {
    .name          = TYPE_BIONZ_HIBARI,
    .parent        = TYPE_SSI_SLAVE,
    .instance_size = sizeof(HibariState),
    .class_init    = hibari_class_init,
};

static void hibari_register_type(void)
{
    type_register_static(&hibari_info);
}

type_init(hibari_register_type)
