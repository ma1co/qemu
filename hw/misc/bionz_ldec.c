/* QEMU model of the Sony hardware lz77 decompressor (ldec) */

#include "qemu/osdep.h"
#include "hw/hw.h"
#include "hw/sysbus.h"
#include "qemu/log.h"
#include "qemu/lz77.h"

#define LDEC_CTRL     0x00
#define LDEC_MODE     0x04
#define LDEC_WRCTRL   0x08
#define LDEC_RDCTRL   0x0c
#define LDEC_RDCTRL2  0x10
#define LDEC_INT      0x14
#define LDEC_INTENBL  0x18
#define LDEC_INTCLEAR 0x1C

#define LDEC_CTRL_ENABLE (1 << 1)

#define TYPE_BIONZ_LDEC "bionz_ldec"
#define BIONZ_LDEC(obj) OBJECT_CHECK(LdecState, (obj), TYPE_BIONZ_LDEC)

typedef struct LdecState {
    SysBusDevice parent_obj;
    MemoryRegion container;
    MemoryRegion mmio;
    MemoryRegion fifo;

    uint32_t reg_ctrl;

    unsigned char *input_buf;
    size_t input_buf_size;
    size_t input_size;

    unsigned char *output_buf;
    size_t output_off;
    size_t output_size;
} LdecState;

static void ldec_reset(DeviceState *dev)
{
    LdecState *s = BIONZ_LDEC(dev);

    s->reg_ctrl = 0;

    if (s->input_buf) {
        g_free(s->input_buf);
        s->input_buf = NULL;
    }
    s->input_buf_size = 0;
    s->input_size = 0;

    if (s->output_buf) {
        g_free(s->output_buf);
        s->output_buf = NULL;
    }
    s->output_off = 0;
    s->output_size = 0;
}

static void ldec_run(LdecState *s)
{
    unsigned char *src;
    int res;

    if (!s->input_size) {
        return;
    }

    src = s->input_buf;
    while (src < s->input_buf + s->input_size) {
        s->output_buf = realloc(s->output_buf, s->output_size + 0x1000);
        res = lz77_inflate(src, s->input_buf + s->input_size - src, s->output_buf + s->output_size, 0x1000, &src);
        if (res < 0) {
            hw_error("%s: lz77_inflate failed\n", __func__);
        }
        s->output_size += res;
    }

    g_free(s->input_buf);
    s->input_buf = NULL;
    s->input_buf_size = 0;
    s->input_size = 0;
}

static uint64_t ldec_read(void *opaque, hwaddr offset, unsigned size)
{
    LdecState *s = BIONZ_LDEC(opaque);

    switch (offset) {
        case LDEC_CTRL:
            return s->reg_ctrl;

        case LDEC_RDCTRL:
            if (s->output_size - s->output_off) {
                return (((s->output_size - s->output_off) & 0x3f) << 8) + 0x10;
            }
            return 0;

        default:
            qemu_log_mask(LOG_UNIMP, "%s: unimplemented read @ 0x%" HWADDR_PRIx "\n", __func__, offset);
            return 0;
    }
}

static void ldec_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    LdecState *s = BIONZ_LDEC(opaque);

    switch (offset) {
        case LDEC_CTRL:
            s->reg_ctrl = value;
            if (!(value & LDEC_CTRL_ENABLE)) {
                ldec_reset(DEVICE(s));
            }
            break;

        default:
            qemu_log_mask(LOG_UNIMP, "%s: unimplemented write @ 0x%" HWADDR_PRIx ": 0x%" PRIx64 "\n", __func__, offset, value);
    }
}

static const struct MemoryRegionOps ldec_ops = {
    .read = ldec_read,
    .write = ldec_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static uint64_t ldec_fifo_read(void *opaque, hwaddr offset, unsigned size)
{
    uint64_t value;
    size_t sz;
    LdecState *s = BIONZ_LDEC(opaque);

    if (!(s->reg_ctrl & LDEC_CTRL_ENABLE)) {
        hw_error("%s: not enabled\n", __func__);
    }

    ldec_run(s);

    value = 0;
    sz = MIN(size, s->output_size - s->output_off);
    memcpy(&value, s->output_buf + s->output_off, sz);
    s->output_off += sz;

    return value;
}

static void ldec_fifo_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    LdecState *s = BIONZ_LDEC(opaque);

    if (!(s->reg_ctrl & LDEC_CTRL_ENABLE)) {
        hw_error("%s: not enabled\n", __func__);
    }

    if (s->input_size + size > s->input_buf_size) {
        s->input_buf_size += 0x1000;
        s->input_buf = g_realloc(s->input_buf, s->input_buf_size);
    }

    memcpy(s->input_buf + s->input_size, &value, size);
    s->input_size += size;
}

static const struct MemoryRegionOps ldec_fifo_ops = {
    .read = ldec_fifo_read,
    .write = ldec_fifo_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static void ldec_realize(DeviceState *dev, Error **errp)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    LdecState *s = BIONZ_LDEC(dev);

    memory_region_init(&s->container, OBJECT(dev), TYPE_BIONZ_LDEC, 0x8000);
    sysbus_init_mmio(sbd, &s->container);

    memory_region_init_io(&s->mmio, OBJECT(dev), &ldec_ops, s, TYPE_BIONZ_LDEC ".mmio", 0x20);
    memory_region_add_subregion(&s->container, 0, &s->mmio);

    memory_region_init_io(&s->fifo, OBJECT(dev), &ldec_fifo_ops, s, TYPE_BIONZ_LDEC ".fifo", 0x4);
    memory_region_add_subregion(&s->container, 0x4000, &s->fifo);
}

static void ldec_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = ldec_realize;
    dc->reset = ldec_reset;
}

static const TypeInfo ldec_info = {
    .name          = TYPE_BIONZ_LDEC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(LdecState),
    .class_init    = ldec_class_init,
};

static void ldec_register_type(void)
{
    type_register_static(&ldec_info);
}

type_init(ldec_register_type)
