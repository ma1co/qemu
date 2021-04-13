/* QEMU model of the Sony CXD4108 framebuffer */

#include "qemu/osdep.h"
#include "ui/console.h"
#include "hw/hw.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/sysbus.h"
#include "qemu/log.h"

#define WIDTH 320
#define HEIGHT 240
#define STRIDE (WIDTH * 2)

#define TYPE_BIONZ_VIP "bionz_vip"
#define BIONZ_VIP(obj) OBJECT_CHECK(VipState, (obj), TYPE_BIONZ_VIP)

typedef struct VipLayer {
    bool enable;
    uint32_t addr;
} VipLayer;

typedef struct VipState {
    SysBusDevice parent_obj;
    MemoryRegion mmio[2];
    qemu_irq irqs[2];
    QemuConsole *con;

    MemoryRegion *memory;
    VipLayer layer;
    uint32_t background;

    uint32_t reg_ch_intsts;
    uint32_t reg_ch_inten;

    uint32_t reg_ctrl;
    uint32_t reg_addr;
    uint32_t reg_num_cpy;
    uint32_t reg_num_repeat;

    uint32_t field;
    uint32_t reg_ctrl_intsts;
    uint32_t reg_ctrl_en;
    uint32_t reg_bg;
} VipState;

static uint32_t ycbcr_to_argb8888(uint8_t y, uint8_t cb, uint8_t cr)
{
    uint8_t r = min(max(y + ((                       91881 * (cr - 0x80) + 0x8000) >> 16), 0), 0xff);
    uint8_t g = min(max(y - (( 22554 * (cb - 0x80) + 46802 * (cr - 0x80) + 0x8000) >> 16), 0), 0xff);
    uint8_t b = min(max(y + ((116130 * (cb - 0x80)                       + 0x8000) >> 16), 0), 0xff);
    return (0xff << 24) | (r << 16) | (g << 8) | b;
}

static uint32_t rgba4444_to_argb8888(uint16_t pix)
{
    uint8_t r = (pix >> 12) & 0xf;
    uint8_t g = (pix >>  8) & 0xf;
    uint8_t b = (pix >>  4) & 0xf;
    uint8_t a =  pix        & 0xf;
    return (a << 28) | (a << 24) | (r << 20) | (r << 16) | (g << 12) | (g << 8) | (b << 4) | b;
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
    DisplaySurface *surface = qemu_console_surface(s->con);
    void *src = memory_region_get_ram_ptr(s->memory);
    uint32_t *dst = surface_data(surface);
    DirtyBitmapSnapshot *snap = NULL;
    int first = -1, last = -1;

    assert(surface_format(surface) == PIXMAN_x8r8g8b8);

    if (s->layer.enable) {
        snap = memory_region_snapshot_and_clear_dirty(s->memory, s->layer.addr, HEIGHT * STRIDE, DIRTY_MEMORY_VGA);
    }

    for (y = 0; y < HEIGHT; y++) {
        if (invalidate || (s->layer.enable && memory_region_snapshot_get_dirty(s->memory, snap, s->layer.addr + y * STRIDE, STRIDE))) {
            for (x = 0; x < WIDTH; x++) {
                dst[x] = s->background;
                if (s->layer.enable) {
                    dst[x] = blend_pixel(dst[x], rgba4444_to_argb8888(*(uint16_t *) (src + s->layer.addr + y * STRIDE + x * 2)));
                }
            }
            if (first < 0) {
                first = y;
            }
            last = y;
        }
        dst += WIDTH;
    }

    g_free(snap);

    if (first >= 0) {
        dpy_gfx_update(s->con, 0, first, WIDTH, last - first + 1);
    }
}

static void vip_update_display(VipState *s)
{
    bool invalidate = false;

    uint32_t bg = (s->reg_bg >> 24) == 0x80 ? ycbcr_to_argb8888((s->reg_bg >> 16) & 0xff, (s->reg_bg >> 8) & 0xff, s->reg_bg & 0xff) : 0;
    if (bg != s->background) {
        s->background = bg;
        invalidate = true;
    }

    VipLayer layer = {0};
    layer.enable = s->reg_ctrl & 1;

    if (layer.enable) {
        if (s->reg_num_cpy != WIDTH * 2 || s->reg_num_repeat != HEIGHT - 1) {
            hw_error("%s: Unsupported image format\n", __func__);
        }
        layer.addr = s->reg_addr;
        if (layer.addr + HEIGHT * STRIDE > memory_region_size(s->memory)) {
            layer = (VipLayer) {0};
        }
    }

    if (memcmp(&layer, &s->layer, sizeof(layer))) {
        s->layer = layer;
        invalidate = true;
    }

    vip_draw(s, invalidate);
}

static void vip_vsync(void *opaque, int irq, int level)
{
    VipState *s = BIONZ_VIP(opaque);

    if (level) {
        vip_update_display(s);
    }

    s->field = level;
    s->reg_ctrl_intsts |= s->reg_ctrl_en & 0x100;

    if (s->reg_ctrl & 1) {
        s->reg_ctrl &= ~1;
        s->reg_ch_intsts |= 0x100;
    }

    vip_update_irq(s);
}

static uint64_t vip_read(void *opaque, hwaddr offset, unsigned size)
{
    VipState *s = BIONZ_VIP(opaque);

    switch (offset) {
        case 0:
            return s->reg_ch_intsts;

        case 8:
            return s->reg_ch_inten;

        case 0x300:
            return s->reg_ctrl;

        case 0x320:
            return s->reg_addr;

        case 0x324:
            return s->reg_num_cpy;

        case 0x32c:
            return s->reg_num_repeat;

        default:
            qemu_log_mask(LOG_UNIMP, "%s: unimplemented read @ 0x%" HWADDR_PRIx "\n", __func__, offset);
            return 0;
    }
}

static void vip_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    VipState *s = BIONZ_VIP(opaque);

    switch (offset) {
        case 0:
            s->reg_ch_intsts &= ~value;
            vip_update_irq(s);
            break;

        case 8:
            s->reg_ch_inten = value;
            vip_update_irq(s);
            break;

        case 0x300:
            s->reg_ctrl = value;
            break;

        case 0x320:
            s->reg_addr = value;
            break;

        case 0x324:
            s->reg_num_cpy = value;
            break;

        case 0x32c:
            s->reg_num_repeat = value;
            break;

        default:
            qemu_log_mask(LOG_UNIMP, "%s: unimplemented write @ 0x%" HWADDR_PRIx ": 0x%" PRIx64 "\n", __func__, offset, value);
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
    VipState *s = BIONZ_VIP(dev);

    s->reg_ch_intsts = 0;
    s->reg_ch_inten = 0;

    s->field = 0;
    s->reg_ctrl_intsts = 0;
    s->reg_ctrl_en = 0;
    s->reg_bg = 0;

    s->reg_ctrl = 0;
    s->reg_addr = 0;
    s->reg_num_cpy = 0;
    s->reg_num_repeat = 0;

    s->layer = (VipLayer) {0};
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
