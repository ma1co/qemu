/* QEMU model of the Sony CXD4108 blit engine */

#include "qemu/osdep.h"
#include "hw/hw.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/sysbus.h"
#include "qemu/log.h"

#define NUM_CHANNELS 3

#define TYPE_BIONZ_CPYFB "bionz_cpyfb"
#define BIONZ_CPYFB(obj) OBJECT_CHECK(CpyfbState, (obj), TYPE_BIONZ_CPYFB)

typedef struct CpyfbChannel {
    uint32_t ctrl;
    uint32_t data;
    uint32_t addr;
    uint32_t num_cpy;
    int32_t num_skip;
    uint32_t num_repeat;
} CpyfbChannel;

typedef struct CpyfbState {
    SysBusDevice parent_obj;
    MemoryRegion mmio[2];
    qemu_irq irq;

    uint32_t mem_base;
    CpyfbChannel channels[NUM_CHANNELS];

    uint32_t reg_intsts;
    uint32_t reg_inten;

    uint32_t reg_ctrl;
    uint32_t reg_alpha_low;
    uint32_t reg_alpha_high;
} CpyfbState;

static uint16_t blend_pixel(uint16_t dst, uint16_t src, uint8_t alpha)
{
    uint8_t rd = (dst >> 12) & 0xf;
    uint8_t gd = (dst >>  8) & 0xf;
    uint8_t bd = (dst >>  4) & 0xf;
    uint8_t ad =  dst        & 0xf;

    uint8_t rs = (src >> 12) & 0xf;
    uint8_t gs = (src >>  8) & 0xf;
    uint8_t bs = (src >>  4) & 0xf;
    uint8_t as =  src        & 0xf;

    uint8_t sc = alpha * as / 0xf;

    uint8_t ro = (rs * sc + rd * (0xf - sc)) / 0xf;
    uint8_t go = (gs * sc + gd * (0xf - sc)) / 0xf;
    uint8_t bo = (bs * sc + bd * (0xf - sc)) / 0xf;
    uint8_t ao = (as * sc + ad * (0xf - sc)) / 0xf;

    return (ro << 12) | (go << 8) | (bo << 4) | ao;
}

static void cpyfb_fill_rect(int dst_stride, hwaddr dst, unsigned int width, unsigned int height, uint16_t rgba)
{
    int i;
    uint16_t *buffer;

    if (width && height) {
        buffer = g_new(uint16_t, width);
        for (i = 0; i < width; i++) {
            buffer[i] = rgba;
        }

        for (i = 0; i < height; i++) {
            cpu_physical_memory_write(dst, buffer, width * sizeof(uint16_t));
            dst += dst_stride;
        }

        g_free(buffer);
    }
}

static void cpyfb_bit_blit(int src_stride, hwaddr src, int dst_stride, hwaddr dst, unsigned int width, unsigned int height)
{
    int i;
    uint16_t *buffer;

    if (width && height) {
        buffer = g_new(uint16_t, width);
        for (i = 0; i < height; i++) {
            cpu_physical_memory_read(src, buffer, width * sizeof(uint16_t));
            cpu_physical_memory_write(dst, buffer, width * sizeof(uint16_t));
            src += src_stride;
            dst += dst_stride;
        }
        g_free(buffer);
    }
}

static void cpyfb_alpha_blend_blit_rgba(int src_stride, hwaddr src, int dst_stride, hwaddr dst, unsigned int width, unsigned int height, uint8_t alpha)
{
    int i, j;
    uint16_t *src_buffer, *dst_buffer;

    if (width && height) {
        src_buffer = g_new(uint16_t, width);
        dst_buffer = g_new(uint16_t, width);
        for (i = 0; i < height; i++) {
            cpu_physical_memory_read(src, src_buffer, width * sizeof(uint16_t));
            cpu_physical_memory_read(dst, dst_buffer, width * sizeof(uint16_t));
            for (j = 0; j < width; j++) {
                dst_buffer[j] = blend_pixel(dst_buffer[j], src_buffer[j], alpha);
            }
            cpu_physical_memory_write(dst, dst_buffer, width * sizeof(uint16_t));
            src += src_stride;
            dst += dst_stride;
        }
        g_free(src_buffer);
        g_free(dst_buffer);
    }
}

static void cpyfb_alpha_blit_rgba(int src_stride, hwaddr src, int dst_stride, hwaddr dst, unsigned int width, unsigned int height)
{
    cpyfb_alpha_blend_blit_rgba(src_stride, src, dst_stride, dst, width, height, 0xf);
}

static void cpyfb_update_irq(CpyfbState *s)
{
    qemu_set_irq(s->irq, !!(s->reg_inten & s->reg_intsts));
}

static void cpyfb_command(CpyfbState *s)
{
    unsigned int i;
    CpyfbChannel *tmp, *src, *dst;
    int ch_en = 0;
    for (i = 0; i < NUM_CHANNELS; i++) {
        if (s->channels[i].ctrl & 1) {
            ch_en |= (1 << i);
        }
    }

    if (ch_en == 2 && s->channels[1].ctrl == 0x21) {
        dst = &s->channels[1];
        if ((dst->data >> 16) != (dst->data & 0xffff)) {
            hw_error("%s: invalid data: 0x%x\n", __func__, dst->data);
        }
        cpyfb_fill_rect(dst->num_cpy + dst->num_skip, s->mem_base + dst->addr,
                        dst->num_cpy / 2, dst->num_repeat + 1,
                        dst->data & 0xffff);
    } else if (ch_en == 3 && s->reg_ctrl == 0x11100001) {
        src = &s->channels[0];
        dst = &s->channels[1];
        if (src->num_cpy != dst->num_cpy || src->num_repeat != dst->num_repeat) {
            hw_error("%s: src size != dst size\n", __func__);
        }
        cpyfb_bit_blit(src->num_cpy + src->num_skip, s->mem_base + src->addr,
                       dst->num_cpy + dst->num_skip, s->mem_base + dst->addr,
                       dst->num_cpy / 2, dst->num_repeat + 1);
    } else if (ch_en == 7) {
        tmp = &s->channels[0];
        dst = &s->channels[1];
        src = &s->channels[2];
        if (tmp->addr != dst->addr || tmp->num_cpy != dst->num_cpy || tmp->num_skip != dst->num_skip || tmp->num_repeat != dst->num_repeat) {
            hw_error("%s: tmp != dst\n", __func__);
        }
        if (src->num_cpy != dst->num_cpy || src->num_repeat != dst->num_repeat) {
            hw_error("%s: src size != dst size\n", __func__);
        }
        if (s->reg_ctrl == 0x10010101) {
            cpyfb_alpha_blit_rgba(src->num_cpy + src->num_skip, s->mem_base + src->addr,
                                  dst->num_cpy + dst->num_skip, s->mem_base + dst->addr,
                                  dst->num_cpy / 2, dst->num_repeat + 1);
        } else if (s->reg_ctrl == 0x10000301) {
            cpyfb_alpha_blend_blit_rgba(src->num_cpy + src->num_skip, s->mem_base + src->addr,
                                        dst->num_cpy + dst->num_skip, s->mem_base + dst->addr,
                                        dst->num_cpy / 2, dst->num_repeat + 1,
                                        s->reg_alpha_high >> 28);
        } else {
            hw_error("%s: Unsupported command\n", __func__);
        }
    } else {
        hw_error("%s: Unsupported command\n", __func__);
    }

    for (i = 0; i < NUM_CHANNELS; i++) {
        if (s->channels[i].ctrl & 1) {
            s->reg_intsts |= 1 << (i * 4);
            s->channels[i].ctrl &= ~1;
        }
    }
    cpyfb_update_irq(s);
}

static uint64_t cpyfb_ch_read(CpyfbState *s, unsigned int ch, hwaddr offset, unsigned size)
{
    CpyfbChannel *channel = &s->channels[ch];

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

static void cpyfb_ch_write(CpyfbState *s, unsigned int ch, hwaddr offset, uint64_t value, unsigned size)
{
    CpyfbChannel *channel = &s->channels[ch];

    switch (offset) {
        case 0x00:
            channel->ctrl = value;
            if (ch == 1 && (value & 1)) {
                cpyfb_command(s);
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

static uint64_t cpyfb_read(void *opaque, hwaddr offset, unsigned size)
{
    CpyfbState *s = BIONZ_CPYFB(opaque);

    if (offset >= 0x200 && offset < (0x200 + NUM_CHANNELS * 0x80)) {
        return cpyfb_ch_read(s, (offset - 0x200) >> 7, offset & 0x7f, size);
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

static void cpyfb_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    CpyfbState *s = BIONZ_CPYFB(opaque);

    if (offset >= 0x200 && offset < (0x200 + NUM_CHANNELS * 0x80)) {
        cpyfb_ch_write(s, (offset - 0x200) >> 7, offset & 0x7f, value, size);
    } else {
        switch (offset) {
            case 0:
                s->reg_intsts &= ~value;
                cpyfb_update_irq(s);
                break;

            case 8:
                s->reg_inten = value;
                cpyfb_update_irq(s);
                break;

            default:
                qemu_log_mask(LOG_UNIMP, "%s: unimplemented write @ 0x%" HWADDR_PRIx ": 0x%" PRIx64 "\n", __func__, offset, value);
        }
    }
}

static uint64_t cpyfb_ctrl_read(void *opaque, hwaddr offset, unsigned size)
{
    CpyfbState *s = BIONZ_CPYFB(opaque);

    switch (offset) {
        case 0x14:
            return s->reg_ctrl;

        case 0x20:
            return s->reg_alpha_low;

        case 0x24:
            return s->reg_alpha_high;

        default:
            qemu_log_mask(LOG_UNIMP, "%s: unimplemented read @ 0x%" HWADDR_PRIx "\n", __func__, offset);
            return 0;
    }
}

static void cpyfb_ctrl_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    CpyfbState *s = BIONZ_CPYFB(opaque);

    switch (offset) {
        case 0x14:
            s->reg_ctrl = value;
            break;

        case 0x20:
            s->reg_alpha_low = value;
            break;

        case 0x24:
            s->reg_alpha_high = value;
            break;

        default:
            qemu_log_mask(LOG_UNIMP, "%s: unimplemented write @ 0x%" HWADDR_PRIx ": 0x%" PRIx64 "\n", __func__, offset, value);
    }
}

static const struct MemoryRegionOps cpyfb_mmio0_ops = {
    .read = cpyfb_read,
    .write = cpyfb_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static const struct MemoryRegionOps cpyfb_mmio1_ops = {
    .read = cpyfb_ctrl_read,
    .write = cpyfb_ctrl_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void cpyfb_reset(DeviceState *dev)
{
    int i;
    CpyfbState *s = BIONZ_CPYFB(dev);

    s->reg_intsts = 0;
    s->reg_inten = 0;

    s->reg_ctrl = 0;
    s->reg_alpha_low = 0;
    s->reg_alpha_high = 0;

    for (i = 0; i < NUM_CHANNELS; i++) {
        s->channels[i].ctrl = 0;
        s->channels[i].data = 0;
        s->channels[i].addr = 0;
        s->channels[i].num_cpy = 0;
        s->channels[i].num_skip = 0;
        s->channels[i].num_repeat = 0;
    }
}

static void cpyfb_realize(DeviceState *dev, Error **errp)
{
    CpyfbState *s = BIONZ_CPYFB(dev);

    memory_region_init_io(&s->mmio[0], OBJECT(dev), &cpyfb_mmio0_ops, s, TYPE_BIONZ_CPYFB ".mmio0", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio[0]);

    memory_region_init_io(&s->mmio[1], OBJECT(dev), &cpyfb_mmio1_ops, s, TYPE_BIONZ_CPYFB ".mmio1", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio[1]);

    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}

static Property cpyfb_properties[] = {
    DEFINE_PROP_UINT32("base", CpyfbState, mem_base, 0),
    DEFINE_PROP_END_OF_LIST(),
};

static void cpyfb_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = cpyfb_realize;
    dc->reset = cpyfb_reset;
    device_class_set_props(dc, cpyfb_properties);
}

static const TypeInfo cpyfb_info = {
    .name          = TYPE_BIONZ_CPYFB,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(CpyfbState),
    .class_init    = cpyfb_class_init,
};

static void cpyfb_register_type(void)
{
    type_register_static(&cpyfb_info);
}

type_init(cpyfb_register_type)
