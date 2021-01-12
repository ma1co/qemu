/* QEMU model of the Sony CXD4108 framebuffer */

#include "qemu/osdep.h"
#include "exec/address-spaces.h"
#include "ui/console.h"
#include "framebuffer.h"
#include "hw/hw.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/sysbus.h"
#include "qemu/log.h"
#include "ui/pixel_ops.h"
#include "sysemu/sysemu.h"

#define WIDTH 320
#define HEIGHT 240
#define PERIOD_NS 33000000 // not sure...

#define TYPE_BIONZ_VIP "bionz_vip"
#define BIONZ_VIP(obj) OBJECT_CHECK(VipState, (obj), TYPE_BIONZ_VIP)

typedef struct VipState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    qemu_irq irq;

    QEMUTimer *timer;
    QemuConsole *con;
    MemoryRegionSection fbsection;

    bool initialized;
    bool invalidate;
    uint32_t mem_base;

    uint32_t reg_addr;
    uint32_t reg_toggle;
    uint32_t reg_inten;
    uint32_t reg_ints;
} VipState;

static void vip_set_timer(VipState *s)
{
    timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + PERIOD_NS);
}

static void vip_update(VipState *s)
{
    qemu_set_irq(s->irq, !!(s->reg_ints & s->reg_inten));
}

static void vip_tick(void *opaque)
{
    VipState *s = BIONZ_VIP(opaque);

    s->reg_toggle = !s->reg_toggle;
    s->reg_ints |= 0x100;
    vip_update(s);
    vip_set_timer(s);
}

static uint64_t vip_read(void *opaque, hwaddr offset, unsigned size)
{
    VipState *s = BIONZ_VIP(opaque);

    switch (offset) {
        case 0x320:
            return s->reg_addr;

        case 0x924:
            return s->reg_toggle;

        case 0x928:
            return 1 << 28;

        case 0x9f8:
            return s->reg_ints;

        case 0x9fc:
            return s->reg_inten;

        default:
            qemu_log_mask(LOG_UNIMP, "%s: unimplemented read @ 0x%" HWADDR_PRIx "\n", __func__, offset);
            return 0;
    }
}

static void vip_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    VipState *s = BIONZ_VIP(opaque);

    switch (offset) {
        case 0x320:
            s->reg_addr = value;
            s->initialized = true;
            s->invalidate = true;
            break;

        case 0x9f8:
            s->reg_ints &= ~value;
            vip_update(s);
            break;

        case 0x9fc:
            s->reg_inten = value;
            vip_update(s);
            break;

        default:
            qemu_log_mask(LOG_UNIMP, "%s: unimplemented write @ 0x%" HWADDR_PRIx ": 0x%" PRIx64 "\n", __func__, offset, value);
    }
}

static const struct MemoryRegionOps vip_ops = {
    .read = vip_read,
    .write = vip_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void vip_draw_line(void *opaque, uint8_t *dst, const uint8_t *src, int width, int step)
{
    uint16_t rgba;
    uint8_t r, g, b;

    while (width--) {
        rgba = lduw_le_p(src);
        src += 2;

        r = (rgba >> 12) & 0xf;
        g = (rgba >>  8) & 0xf;
        b = (rgba >>  4) & 0xf;

        *(uint32_t *) dst = rgb_to_pixel32(r * 0x11, g * 0x11, b * 0x11);
        dst += 4;
    }
}

static void vip_display_update(void *opaque)
{
    int first = 0, last;

    VipState *s = BIONZ_VIP(opaque);
    DisplaySurface *surface = qemu_console_surface(s->con);

    if (surface_format(surface) != PIXMAN_x8r8g8b8) {
        hw_error("%s: Unsupported surface format\n", __func__);
    }

    if (!s->initialized) {
        return;
    }

    if (s->invalidate) {
        framebuffer_update_memory_section(&s->fbsection, get_system_memory(), s->mem_base + s->reg_addr, HEIGHT, WIDTH * 2);
    }

    framebuffer_update_display(surface, &s->fbsection, WIDTH, HEIGHT, WIDTH * 2, WIDTH * 4, 0, s->invalidate, vip_draw_line, s, &first, &last);

    if (first >= 0) {
        dpy_gfx_update(s->con, 0, first, WIDTH, last - first + 1);
    }

    s->invalidate = false;
}

static const GraphicHwOps vip_gfx_ops = {
    .gfx_update = vip_display_update,
};

static void vip_reset(DeviceState *dev)
{
    VipState *s = BIONZ_VIP(dev);

    s->initialized = false;
    s->invalidate = false;

    s->reg_addr = 0;
    s->reg_toggle = 0;
    s->reg_inten = 0;
    s->reg_ints = 0;

    timer_del(s->timer);
    vip_set_timer(s);
}

static void vip_realize(DeviceState *dev, Error **errp)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    VipState *s = BIONZ_VIP(dev);

    memory_region_init_io(&s->mmio, OBJECT(dev), &vip_ops, s, TYPE_BIONZ_VIP, 0x1000);
    sysbus_init_mmio(sbd, &s->mmio);
    sysbus_init_irq(sbd, &s->irq);

    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, vip_tick, s);

    s->con = graphic_console_init(dev, 0, &vip_gfx_ops, s);
    qemu_console_resize(s->con, WIDTH, HEIGHT);
}

static Property vip_properties[] = {
    DEFINE_PROP_UINT32("base", VipState, mem_base, 0),
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
