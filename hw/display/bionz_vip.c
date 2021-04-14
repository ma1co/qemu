/* QEMU model of the Sony CXD4108 framebuffer */

#include "qemu/osdep.h"
#include "ui/console.h"
#include "hw/hw.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/sysbus.h"
#include "qemu/log.h"

#define NUM_CHANNELS 3
#define NUM_LAYERS 2

#define WIDTH 320
#define HEIGHT 240

#define TYPE_BIONZ_VIP "bionz_vip"
#define BIONZ_VIP(obj) OBJECT_CHECK(VipState, (obj), TYPE_BIONZ_VIP)

typedef enum VipFormat {
    FORMAT_RGBA4444,
    FORMAT_YCBCR422,
    FORMAT_YCBCR422_DOWNSIZE,
} VipFormat;

const uint32_t vip_strides[] = {
    [FORMAT_RGBA4444] = WIDTH * 2,
    [FORMAT_YCBCR422] = WIDTH * 2,
    [FORMAT_YCBCR422_DOWNSIZE] = WIDTH * 8,
};

typedef struct VipChannel {
    uint32_t ctrl;
    uint32_t addr;
    uint32_t num_cpy;
    uint32_t num_repeat;
} VipChannel;

typedef struct VipLayer {
    bool enable;
    VipFormat format;
    uint32_t addr;
    DirtyBitmapSnapshot *snap;
} VipLayer;

typedef struct VipState {
    SysBusDevice parent_obj;
    MemoryRegion mmio[2];
    qemu_irq irqs[2];
    QemuConsole *con;

    MemoryRegion *memory;

    VipChannel channels[NUM_CHANNELS];
    VipLayer layers[NUM_LAYERS];
    uint32_t background;

    uint32_t reg_ch_intsts;
    uint32_t reg_ch_inten;

    uint32_t field;
    uint32_t reg_ctrl_intsts;
    uint32_t reg_ctrl_en;
    uint32_t reg_bg;
} VipState;

static uint32_t ycbcr_to_argb8888(uint8_t y, uint8_t cb, uint8_t cr)
{
    uint8_t r = MIN(MAX(y + ((                       91881 * (cr - 0x80) + 0x8000) >> 16), 0), 0xff);
    uint8_t g = MIN(MAX(y - (( 22554 * (cb - 0x80) + 46802 * (cr - 0x80) + 0x8000) >> 16), 0), 0xff);
    uint8_t b = MIN(MAX(y + ((116130 * (cb - 0x80)                       + 0x8000) >> 16), 0), 0xff);
    return (0xff << 24) | (r << 16) | (g << 8) | b;
}

static uint32_t ycbcr422_to_argb8888(uint32_t pix, bool off)
{
    uint8_t y1 = (pix >> 24) & 0xff;
    uint8_t cr = (pix >> 16) & 0xff;
    uint8_t y0 = (pix >>  8) & 0xff;
    uint8_t cb =  pix        & 0xff;
    return ycbcr_to_argb8888(off ? y1 : y0, cb, cr);
}

static uint32_t rgba4444_to_argb8888(uint16_t pix)
{
    uint8_t r = (pix >> 12) & 0xf;
    uint8_t g = (pix >>  8) & 0xf;
    uint8_t b = (pix >>  4) & 0xf;
    uint8_t a =  pix        & 0xf;
    return (a << 28) | (a << 24) | (r << 20) | (r << 16) | (g << 12) | (g << 8) | (b << 4) | b;
}

static uint32_t get_pixel(const void *src, VipFormat format, unsigned int x)
{
    switch (format) {
        case FORMAT_RGBA4444:
            return rgba4444_to_argb8888(*(uint16_t *) (src + x * 2));

        case FORMAT_YCBCR422_DOWNSIZE:
            x *= 2;
            // fall-through

        case FORMAT_YCBCR422:
            return ycbcr422_to_argb8888(*(uint32_t *) (src + (x >> 1) * 4), x & 1);

        default:
            return 0;
    }
}

static uint32_t blend_pixel(uint32_t dst, uint32_t src)
{
    uint8_t rd = (dst >> 16) & 0xff;
    uint8_t gd = (dst >>  8) & 0xff;
    uint8_t bd =  dst        & 0xff;

    uint8_t rs = (src >> 16) & 0xff;
    uint8_t gs = (src >>  8) & 0xff;
    uint8_t bs =  src        & 0xff;

    uint8_t sc = (src >> 24) & 0xff;

    uint8_t ro = (rs * sc + rd * (0xff - sc)) / 0xff;
    uint8_t go = (gs * sc + gd * (0xff - sc)) / 0xff;
    uint8_t bo = (bs * sc + bd * (0xff - sc)) / 0xff;

    return (0xff << 24) | (ro << 16) | (go << 8) |  bo;
}

static void vip_update_irq(VipState *s)
{
    qemu_set_irq(s->irqs[0], !!(s->reg_ctrl_intsts & 0x100));
    qemu_set_irq(s->irqs[1], !!(s->reg_ch_inten & s->reg_ch_intsts));
}

static void vip_draw(VipState *s, bool invalidate)
{
    unsigned int x, y;
    VipLayer *l;
    DisplaySurface *surface = qemu_console_surface(s->con);
    void *src = memory_region_get_ram_ptr(s->memory);
    uint32_t *dst = surface_data(surface);
    int first = -1, last = -1;

    assert(surface_format(surface) == PIXMAN_x8r8g8b8);

    for (l = &s->layers[0]; l < &s->layers[NUM_LAYERS]; l++) {
        if (l->enable) {
            l->snap = memory_region_snapshot_and_clear_dirty(s->memory, l->addr, HEIGHT * vip_strides[l->format], DIRTY_MEMORY_VGA);
        }
    }

    for (y = 0; y < HEIGHT; y++) {
        bool update = invalidate;
        for (l = &s->layers[0]; l < &s->layers[NUM_LAYERS]; l++) {
            if (l->enable) {
                update = update || memory_region_snapshot_get_dirty(s->memory, l->snap, l->addr + y * vip_strides[l->format], vip_strides[l->format]);
            }
        }
        if (update) {
            for (x = 0; x < WIDTH; x++) {
                dst[x] = s->background;
                for (l = &s->layers[0]; l < &s->layers[NUM_LAYERS]; l++) {
                    if (l->enable) {
                        dst[x] = blend_pixel(dst[x], get_pixel(src + l->addr + y * vip_strides[l->format], l->format, x));
                    }
                }
            }
            if (first < 0) {
                first = y;
            }
            last = y;
        }
        dst += WIDTH;
    }

    for (l = &s->layers[0]; l < &s->layers[NUM_LAYERS]; l++) {
        g_free(l->snap);
        l->snap = NULL;
    }

    if (first >= 0) {
        dpy_gfx_update(s->con, 0, first, WIDTH, last - first + 1);
    }
}

static void vip_update_display(VipState *s)
{
    unsigned int i;
    bool invalidate = false;

    uint32_t bg = (s->reg_bg >> 24) == 0x80 ? ycbcr_to_argb8888((s->reg_bg >> 16) & 0xff, (s->reg_bg >> 8) & 0xff, s->reg_bg & 0xff) : 0;
    if (bg != s->background) {
        s->background = bg;
        invalidate = true;
    }

    for (i = 0; i < NUM_LAYERS; i++) {
        VipChannel *channel = &s->channels[2 * i];
        VipLayer layer = {0};
        layer.enable = channel->ctrl & 1;

        if (layer.enable) {
            if (channel->num_cpy == WIDTH * 2 && channel->num_repeat == HEIGHT - 1) {
                layer.format = i ? FORMAT_RGBA4444 : FORMAT_YCBCR422;
            } else if (!i && channel->num_cpy == WIDTH * 4 && channel->num_repeat == HEIGHT - 1) {
                layer.format = FORMAT_YCBCR422_DOWNSIZE;
            } else {
                hw_error("%s: Unsupported image format\n", __func__);
            }

            layer.addr = channel->addr;

            if (layer.addr + vip_strides[layer.format] * HEIGHT > memory_region_size(s->memory)) {
                layer = (VipLayer) {0};
            }
        }

        if (memcmp(&layer, &s->layers[i], sizeof(layer))) {
            s->layers[i] = layer;
            invalidate = true;
        }
    }

    vip_draw(s, invalidate);
}

static void vip_vsync(void *opaque, int irq, int level)
{
    unsigned int i;
    VipState *s = BIONZ_VIP(opaque);

    if (level) {
        vip_update_display(s);
    }

    s->field = level;
    s->reg_ctrl_intsts |= s->reg_ctrl_en & 0x100;

    for (i = 0; i < NUM_CHANNELS; i++) {
        if (s->channels[i].ctrl & 1) {
            s->channels[i].ctrl &= ~1;
            s->reg_ch_intsts |= 1 << (4 * i);
        }
    }

    vip_update_irq(s);
}

static uint64_t vip_ch_read(VipState *s, unsigned int ch, hwaddr offset, unsigned size)
{
    VipChannel *channel = &s->channels[ch];

    switch (offset) {
        case 0x00:
            return channel->ctrl;

        case 0x20:
            return channel->addr;

        case 0x24:
            return channel->num_cpy;

        case 0x2c:
            return channel->num_repeat;

        default:
            qemu_log_mask(LOG_UNIMP, "%s: unimplemented channel read @ 0x%" HWADDR_PRIx "\n", __func__, offset);
            return 0;
    }
}

static void vip_ch_write(VipState *s, unsigned int ch, hwaddr offset, uint64_t value, unsigned size)
{
    VipChannel *channel = &s->channels[ch];

    switch (offset) {
        case 0x00:
            channel->ctrl = value;
            break;

        case 0x20:
            channel->addr = value;
            break;

        case 0x24:
            channel->num_cpy = value;
            break;

        case 0x2c:
            channel->num_repeat = value;
            break;

        default:
            qemu_log_mask(LOG_UNIMP, "%s: unimplemented channel write @ 0x%" HWADDR_PRIx ": 0x%" PRIx64 "\n", __func__, offset, value);
    }
}

static uint64_t vip_read(void *opaque, hwaddr offset, unsigned size)
{
    VipState *s = BIONZ_VIP(opaque);

    if (offset >= 0x200 && offset < (0x200 + NUM_CHANNELS * 0x80)) {
        return vip_ch_read(s, (offset - 0x200) >> 7, offset & 0x7f, size);
    } else {
        switch (offset) {
            case 0:
                return s->reg_ch_intsts;

            case 8:
                return s->reg_ch_inten;

            default:
                qemu_log_mask(LOG_UNIMP, "%s: unimplemented read @ 0x%" HWADDR_PRIx "\n", __func__, offset);
                return 0;
        }
    }
}

static void vip_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    VipState *s = BIONZ_VIP(opaque);

    if (offset >= 0x200 && offset < (0x200 + NUM_CHANNELS * 0x80)) {
        vip_ch_write(s, (offset - 0x200) >> 7, offset & 0x7f, value, size);
    } else {
        switch (offset) {
            case 0:
                s->reg_ch_intsts &= ~value;
                vip_update_irq(s);
                break;

            case 8:
                s->reg_ch_inten = value;
                vip_update_irq(s);
                break;

            default:
                qemu_log_mask(LOG_UNIMP, "%s: unimplemented write @ 0x%" HWADDR_PRIx ": 0x%" PRIx64 "\n", __func__, offset, value);
        }
    }
}

static uint64_t vip_ctrl_read(void *opaque, hwaddr offset, unsigned size)
{
    VipState *s = BIONZ_VIP(opaque);

    switch (offset) {
        case 0x124:
            return s->field;

        case 0x12c:
            return s->field ? (1 << 28) : 0;

        case 0x1f8:
            return s->reg_ctrl_intsts;

        case 0x1fc:
            return s->reg_ctrl_en;

        case 0x310:
            return s->reg_bg;

        default:
            qemu_log_mask(LOG_UNIMP, "%s: unimplemented read @ 0x%" HWADDR_PRIx "\n", __func__, offset);
            return 0;
    }
}

static void vip_ctrl_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    VipState *s = BIONZ_VIP(opaque);

    switch (offset) {
        case 0x1f8:
            s->reg_ctrl_intsts &= ~value;
            vip_update_irq(s);
            break;

        case 0x1fc:
            s->reg_ctrl_en = value;
            break;

        case 0x310:
            s->reg_bg = value;
            break;

        default:
            qemu_log_mask(LOG_UNIMP, "%s: unimplemented write @ 0x%" HWADDR_PRIx ": 0x%" PRIx64 "\n", __func__, offset, value);
    }
}

static const struct MemoryRegionOps vip_mmio0_ops = {
    .read = vip_read,
    .write = vip_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static const struct MemoryRegionOps vip_mmio1_ops = {
    .read = vip_ctrl_read,
    .write = vip_ctrl_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static const GraphicHwOps vip_gfx_ops = {};

static void vip_reset(DeviceState *dev)
{
    unsigned int i;
    VipState *s = BIONZ_VIP(dev);

    s->reg_ch_intsts = 0;
    s->reg_ch_inten = 0;

    s->field = 0;
    s->reg_ctrl_intsts = 0;
    s->reg_ctrl_en = 0;
    s->reg_bg = 0;

    for (i = 0; i < NUM_CHANNELS; i++) {
        s->channels[i] = (VipChannel) {0};
    }
    for (i = 0; i < NUM_LAYERS; i++) {
        s->layers[i] = (VipLayer) {0};
    }
    s->background = 0;
}

static void vip_realize(DeviceState *dev, Error **errp)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    VipState *s = BIONZ_VIP(dev);

    memory_region_init_io(&s->mmio[0], OBJECT(dev), &vip_mmio0_ops, s, TYPE_BIONZ_VIP ".mmio0", 0x800);
    sysbus_init_mmio(sbd, &s->mmio[0]);

    memory_region_init_io(&s->mmio[1], OBJECT(dev), &vip_mmio1_ops, s, TYPE_BIONZ_VIP ".mmio1", 0x800);
    sysbus_init_mmio(sbd, &s->mmio[1]);

    sysbus_init_irq(sbd, &s->irqs[0]);
    sysbus_init_irq(sbd, &s->irqs[1]);
    qdev_init_gpio_in(dev, vip_vsync, 1);

    s->con = graphic_console_init(dev, 0, &vip_gfx_ops, s);
    qemu_console_resize(s->con, WIDTH, HEIGHT);

    assert(s->memory && memory_region_is_ram(s->memory));
    memory_region_set_log(s->memory, true, DIRTY_MEMORY_VGA);
}

static Property vip_properties[] = {
    DEFINE_PROP_LINK("memory", VipState, memory, TYPE_MEMORY_REGION, MemoryRegion *),
    DEFINE_PROP_END_OF_LIST(),
};

static void vip_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = vip_realize;
    dc->reset = vip_reset;
    device_class_set_props(dc, vip_properties);
}

static const TypeInfo vip_info = {
    .name          = TYPE_BIONZ_VIP,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(VipState),
    .class_init    = vip_class_init,
};

static void vip_register_type(void)
{
    type_register_static(&vip_info);
}

type_init(vip_register_type)
