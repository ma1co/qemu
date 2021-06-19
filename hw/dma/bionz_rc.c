/* QEMU model of the Sony CXD4108 image resize engine */

#include "qemu/osdep.h"
#include "hw/hw.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/sysbus.h"
#include "qemu/log.h"

#define NUM_CHANNELS 4

#define TYPE_BIONZ_RC "bionz_rc"
#define BIONZ_RC(obj) OBJECT_CHECK(RcState, (obj), TYPE_BIONZ_RC)

typedef struct RcChannel {
    uint32_t ctrl;
    uint32_t data;
    uint32_t addr;
    uint32_t num_cpy;
    int32_t num_skip;
    uint32_t num_repeat;
} RcChannel;

typedef struct RcState {
    SysBusDevice parent_obj;
    MemoryRegion mmio[2];
    qemu_irq irq;

    uint32_t mem_base;
    RcChannel channels[NUM_CHANNELS];

    uint32_t reg_intsts;
    uint32_t reg_inten;

    uint32_t reg_scale[2];
    uint32_t reg_offset[2];
    uint32_t reg_src_dim;
    uint32_t reg_dst_dim;
} RcState;

static void rc_fill(RcState *s, RcChannel *ch)
{
    unsigned int i;
    uint32_t *buffer;

    unsigned int count = ch->num_cpy / sizeof(uint32_t);
    hwaddr dst = s->mem_base + ch->addr;

    buffer = g_new(uint32_t, count);
    for (i = 0; i < count; i++) {
        buffer[i] = ch->data;
    }

    for (i = 0; i <= ch->num_repeat; i++) {
        cpu_physical_memory_write(dst, buffer, count * sizeof(uint32_t));
        dst += ch->num_cpy + ch->num_skip;
    }

    g_free(buffer);
}

static void rc_resize(RcState *s, RcChannel *src, RcChannel *dst)
{
    unsigned int x, y, dst_off, src_off;
    uint32_t *dst_buffer, *src_buffer;

    uint16_t dst_width = (s->reg_dst_dim >> 16) & 0x1fff;
    uint16_t dst_height = s->reg_dst_dim & 0x1fff;
    uint16_t src_width = (s->reg_src_dim >> 16) & 0x1fff;
    uint16_t src_height = s->reg_src_dim & 0x1fff;
    int32_t src_offset_x = (sextract32(s->reg_offset[0], 0, 26) + 0x800) >> 12;
    int32_t src_offset_y = (sextract32(s->reg_offset[1], 0, 26) + 0x800) >> 12;

    if (src_offset_y + (((dst_height - 1) * s->reg_scale[1]) >> 12) >= src_height) {
        hw_error("%s: Invalid height\n", __func__);
    }
    if ((src_offset_x / 2 + (((dst_width / 2 - 1) * s->reg_scale[0]) >> 12)) * 2 + 1 >= src_width) {
        hw_error("%s: Invalid width\n", __func__);
    }

    dst_buffer = g_new(uint32_t, dst_width / 2);
    src_buffer = g_new(uint32_t, src_width / 2);

    for (y = 0; y < dst_height; y++) {
        dst_off = y * (dst->num_cpy + dst->num_skip);
        src_off = (src_offset_y + ((y * s->reg_scale[1]) >> 12)) * (src->num_cpy + src->num_skip);

        cpu_physical_memory_read(s->mem_base + src->addr + src_off, src_buffer, src_width / 2 * sizeof(uint32_t));
        for (x = 0; x < dst_width / 2; x++) {
            dst_buffer[x] = src_buffer[src_offset_x / 2 + ((x * s->reg_scale[0]) >> 12)];
        }
        cpu_physical_memory_write(s->mem_base + dst->addr + dst_off, dst_buffer, dst_width / 2 * sizeof(uint32_t));
    }

    g_free(dst_buffer);
    g_free(src_buffer);
}

static void rc_update_irq(RcState *s)
{
    qemu_set_irq(s->irq, !!(s->reg_inten & s->reg_intsts));
}

static void rc_command(RcState *s)
{
    unsigned int i;
    int ch_en = 0;
    for (i = 0; i < NUM_CHANNELS; i++) {
        if (s->channels[i].ctrl & 1) {
            ch_en |= (1 << i);
        }
    }

    if (ch_en == 0b0010 && s->channels[1].ctrl == 0x21) {
        rc_fill(s, &s->channels[1]);
    } else if (ch_en == 0b1000 && s->channels[3].ctrl == 0x21) {
        rc_fill(s, &s->channels[3]);
    } else if (ch_en == 0b0011) {
        rc_resize(s, &s->channels[0], &s->channels[1]);
    } else if (ch_en == 0b1001) {
        rc_resize(s, &s->channels[0], &s->channels[3]);
    } else if (ch_en == 0b1100) {
        rc_resize(s, &s->channels[2], &s->channels[3]);
    } else {
        hw_error("%s: Unsupported command\n", __func__);
    }

    for (i = 0; i < NUM_CHANNELS; i++) {
        if (s->channels[i].ctrl & 1) {
            s->reg_intsts |= 1 << (i * 4);
            s->channels[i].ctrl &= ~1;
        }
    }
    rc_update_irq(s);
}

static uint64_t rc_ch_read(RcState *s, unsigned int ch, hwaddr offset, unsigned size)
{
    RcChannel *channel = &s->channels[ch];

    switch (offset) {
        case 0x00:
            return channel->ctrl;

        case 0x0c:
            return channel->data;

        case 0x20:
            return channel->addr;

        case 0x24:
            return channel->num_cpy;

        case 0x28:
            return channel->num_skip;

        case 0x2c:
            return channel->num_repeat;

        default:
            qemu_log_mask(LOG_UNIMP, "%s: unimplemented channel read @ 0x%" HWADDR_PRIx "\n", __func__, offset);
            return 0;
    }
}

static void rc_ch_write(RcState *s, unsigned int ch, hwaddr offset, uint64_t value, unsigned size)
{
    RcChannel *channel = &s->channels[ch];

    switch (offset) {
        case 0x00:
            channel->ctrl = value;
            if (((ch == 0 || ch == 2) || ((ch == 1 || ch == 3) && (value & 0x20))) && (value & 1)) {
                rc_command(s);
            }
            break;

        case 0x0c:
            channel->data = value;
            break;

        case 0x20:
            channel->addr = value;
            break;

        case 0x24:
            channel->num_cpy = value;
            break;

        case 0x28:
            channel->num_skip = value;
            break;

        case 0x2c:
            channel->num_repeat = value;
            break;

        default:
            qemu_log_mask(LOG_UNIMP, "%s: unimplemented channel write @ 0x%" HWADDR_PRIx ": 0x%" PRIx64 "\n", __func__, offset, value);
    }
}

static uint64_t rc_read(void *opaque, hwaddr offset, unsigned size)
{
    RcState *s = BIONZ_RC(opaque);

    if (offset >= 0x200 && offset < (0x200 + NUM_CHANNELS * 0x80)) {
        return rc_ch_read(s, (offset - 0x200) >> 7, offset & 0x7f, size);
    } else {
        switch (offset) {
            case 0:
                return s->reg_intsts;

            case 8:
                return s->reg_inten;

            default:
                qemu_log_mask(LOG_UNIMP, "%s: unimplemented read @ 0x%" HWADDR_PRIx "\n", __func__, offset);
                return 0;
        }
    }
}

static void rc_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    RcState *s = BIONZ_RC(opaque);

    if (offset >= 0x200 && offset < (0x200 + NUM_CHANNELS * 0x80)) {
        rc_ch_write(s, (offset - 0x200) >> 7, offset & 0x7f, value, size);
    } else {
        switch (offset) {
            case 0:
                s->reg_intsts &= ~value;
                rc_update_irq(s);
                break;

            case 8:
                s->reg_inten = value;
                rc_update_irq(s);
                break;

            default:
                qemu_log_mask(LOG_UNIMP, "%s: unimplemented write @ 0x%" HWADDR_PRIx ": 0x%" PRIx64 "\n", __func__, offset, value);
        }
    }
}

static uint64_t rc_ctrl_read(void *opaque, hwaddr offset, unsigned size)
{
    RcState *s = BIONZ_RC(opaque);

    switch (offset) {
        case 0x10:
            return s->reg_scale[0];

        case 0x14:
            return s->reg_scale[1];

        case 0x18:
            return s->reg_offset[0];

        case 0x1c:
            return s->reg_offset[1];

        case 0x20:
            return s->reg_src_dim;

        case 0x24:
            return s->reg_dst_dim;

        default:
            qemu_log_mask(LOG_UNIMP, "%s: unimplemented read @ 0x%" HWADDR_PRIx "\n", __func__, offset);
            return 0;
    }
}

static void rc_ctrl_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    RcState *s = BIONZ_RC(opaque);

    switch (offset) {
        case 0x10:
            s->reg_scale[0] = value;
            break;

        case 0x14:
            s->reg_scale[1] = value;
            break;

        case 0x18:
            s->reg_offset[0] = value;
            break;

        case 0x1c:
            s->reg_offset[1] = value;
            break;

        case 0x20:
            s->reg_src_dim = value;
            break;

        case 0x24:
            s->reg_dst_dim = value;
            break;

        default:
            qemu_log_mask(LOG_UNIMP, "%s: unimplemented write @ 0x%" HWADDR_PRIx ": 0x%" PRIx64 "\n", __func__, offset, value);
    }
}

static const struct MemoryRegionOps rc_mmio0_ops = {
    .read = rc_read,
    .write = rc_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static const struct MemoryRegionOps rc_mmio1_ops = {
    .read = rc_ctrl_read,
    .write = rc_ctrl_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void rc_reset(DeviceState *dev)
{
    int i;
    RcState *s = BIONZ_RC(dev);

    s->reg_intsts = 0;
    s->reg_inten = 0;

    s->reg_scale[0] = 0;
    s->reg_scale[1] = 0;
    s->reg_offset[0] = 0;
    s->reg_offset[1] = 0;
    s->reg_src_dim = 0;
    s->reg_dst_dim = 0;

    for (i = 0; i < NUM_CHANNELS; i++) {
        s->channels[i].ctrl = 0;
        s->channels[i].data = 0;
        s->channels[i].addr = 0;
        s->channels[i].num_cpy = 0;
        s->channels[i].num_skip = 0;
        s->channels[i].num_repeat = 0;
    }
}

static void rc_realize(DeviceState *dev, Error **errp)
{
    RcState *s = BIONZ_RC(dev);

    memory_region_init_io(&s->mmio[0], OBJECT(dev), &rc_mmio0_ops, s, TYPE_BIONZ_RC ".mmio0", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio[0]);

    memory_region_init_io(&s->mmio[1], OBJECT(dev), &rc_mmio1_ops, s, TYPE_BIONZ_RC ".mmio1", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio[1]);

    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}

static Property rc_properties[] = {
    DEFINE_PROP_UINT32("base", RcState, mem_base, 0),
    DEFINE_PROP_END_OF_LIST(),
};

static void rc_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = rc_realize;
    dc->reset = rc_reset;
    device_class_set_props(dc, rc_properties);
}

static const TypeInfo rc_info = {
    .name          = TYPE_BIONZ_RC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RcState),
    .class_init    = rc_class_init,
};

static void rc_register_type(void)
{
    type_register_static(&rc_info);
}

type_init(rc_register_type)
