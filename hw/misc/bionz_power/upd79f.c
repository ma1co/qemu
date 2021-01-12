/* QEMU model of the Sony UPD79F0043FC battery authentication chip */

#include "qemu/osdep.h"
#include "hw/hw.h"
#include "hw/ssi/ssi.h"
#include "checksum.h"

#define TYPE_BIONZ_UPD79F "bionz_upd79f"
#define BIONZ_UPD79F(obj) OBJECT_CHECK(Upd79fState, (obj), TYPE_BIONZ_UPD79F)

typedef struct Upd79fState {
    SSISlave parent_obj;
    uint8_t buf[24];
    uint8_t buflen;
    uint8_t pos;
    bool state;
} Upd79fState;

static uint8_t upd79f_get_status(Upd79fState *s)
{
    return 0x15;
}

static uint8_t upd79f_read(Upd79fState *s, uint8_t addr, uint8_t *buf)
{
    uint8_t len = 0;
    switch (addr) {
        case 2:  len =  4; break;
        case 4:  len = 18; break;
        case 6:  len =  4; break;
        default: hw_error("%s: Unknown address: %d", __func__, addr);
    }
    memset(buf, 0, len);
    return len;
}

static void upd79f_write(Upd79fState *s, uint8_t addr, uint8_t *buf, uint8_t len)
{

}

static uint32_t upd79f_transfer(SSISlave *dev, uint32_t value)
{
    Upd79fState *s = BIONZ_UPD79F(dev);
    uint8_t res = 0;
    uint8_t len;

    if (!s->state) { // receive
        s->buf[s->pos] = value;
        if (s->pos == 0 && value != 0xc9) {
            hw_error("%s: 0x%x != 0xc9", __func__, value);
        }
        if (s->pos >= 3 && s->pos == 3 + s->buf[2]) {
            if (value != parity(s->buf, s->pos, 1)) {
                hw_error("%s: Wrong checksum: 0x%x", __func__, value);
            }
            s->buf[s->pos + 1] = upd79f_get_status(s);
            s->buflen = s->pos + 2;
            if (s->buf[2]) {
                upd79f_write(s, s->buf[1], &s->buf[3], s->buf[2]);
            } else {
                len = upd79f_read(s, s->buf[1], &s->buf[s->pos + 2]);
                s->buf[s->pos + 2 + len] = parity(&s->buf[s->pos + 1], len + 1, 1);
                s->buflen += len + 1;
            }
            s->state = true;
        }
    } else { // send
        res = s->buf[s->pos];
    }

    s->pos++;
    if (s->pos >= s->buflen) {
        s->buflen = sizeof(s->buf);
        s->pos = 0;
        s->state = false;
    }

    return res;
}

static void upd79f_realize(SSISlave *dev, Error **errp)
{
    Upd79fState *s = BIONZ_UPD79F(dev);

    s->buflen = sizeof(s->buf);
    s->pos = 0;
    s->state = false;
}

static void upd79f_class_init(ObjectClass *klass, void *data)
{
    SSISlaveClass *k = SSI_SLAVE_CLASS(klass);

    k->realize = upd79f_realize;
    k->transfer = upd79f_transfer;
    k->cs_polarity = SSI_CS_LOW;
}

static const TypeInfo upd79f_info = {
    .name          = TYPE_BIONZ_UPD79F,
    .parent        = TYPE_SSI_SLAVE,
    .instance_size = sizeof(Upd79fState),
    .class_init    = upd79f_class_init,
};

static void upd79f_register_type(void)
{
    type_register_static(&upd79f_info);
}

type_init(upd79f_register_type)
