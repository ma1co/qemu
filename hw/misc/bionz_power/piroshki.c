/* QEMU model of the Sony Piroshki power IC (BU76381GUW) */

#include "qemu/osdep.h"
#include "hw/ssi/ssi.h"
#include "checksum.h"

#define TYPE_BIONZ_PIROSHKI "bionz_piroshki"
#define BIONZ_PIROSHKI(obj) OBJECT_CHECK(PiroshkiState, (obj), TYPE_BIONZ_PIROSHKI)

typedef struct PiroshkiState {
    SSISlave parent_obj;
    uint8_t buf[28];
    uint8_t buf_pos;
} PiroshkiState;

static void piroshki_cmd(PiroshkiState *s)
{
    memset(s->buf, 0, sizeof(s->buf));
    s->buf[22] = parity(s->buf + 2, 19, 1);
}

static uint32_t piroshki_transfer(SSISlave *dev, uint32_t value)
{
    PiroshkiState *s = BIONZ_PIROSHKI(dev);
    uint8_t ret = s->buf[s->buf_pos];
    s->buf[s->buf_pos] = value;
    s->buf_pos++;
    if (s->buf_pos >= sizeof(s->buf)) {
        piroshki_cmd(s);
        s->buf_pos = 0;
    }
    return ret;
}

static void piroshki_realize(SSISlave *dev, Error **errp)
{
    PiroshkiState *s = BIONZ_PIROSHKI(dev);

    piroshki_cmd(s);
    s->buf_pos = 0;
}

static void piroshki_class_init(ObjectClass *klass, void *data)
{
    SSISlaveClass *k = SSI_SLAVE_CLASS(klass);

    k->realize = piroshki_realize;
    k->transfer = piroshki_transfer;
    k->cs_polarity = SSI_CS_LOW;
}

static const TypeInfo piroshki_info = {
    .name          = TYPE_BIONZ_PIROSHKI,
    .parent        = TYPE_SSI_SLAVE,
    .instance_size = sizeof(PiroshkiState),
    .class_init    = piroshki_class_init,
};

static void piroshki_register_type(void)
{
    type_register_static(&piroshki_info);
}

type_init(piroshki_register_type)
