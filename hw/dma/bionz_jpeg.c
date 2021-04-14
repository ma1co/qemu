/* QEMU model of the Sony CXD4108 JPEG decoder */

#include "qemu/osdep.h"
#include "hw/hw.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/sysbus.h"
#include "qemu/log.h"
#include <jpeglib.h>

#define NUM_CHANNELS 3

#define TYPE_BIONZ_JPEG "bionz_jpeg"
#define BIONZ_JPEG(obj) OBJECT_CHECK(JpegState, (obj), TYPE_BIONZ_JPEG)

typedef struct JpegChannel {
    uint32_t ctrl;
    uint32_t data;
    uint32_t addr;
    uint32_t num_cpy;
    int32_t num_skip;
    uint32_t num_repeat;
} JpegChannel;

typedef struct JpegState {
    SysBusDevice parent_obj;
    MemoryRegion mmio[2];
    qemu_irq irq;

    uint32_t mem_base;
    JpegChannel channels[NUM_CHANNELS];

    uint32_t reg_intsts;
    uint32_t reg_inten;

    uint32_t reg_ctrl;
    uint32_t reg_jpeg_offset;
    uint32_t reg_jpeg_size;
    uint32_t reg_jpeg_width;
    uint32_t reg_size_ctrl;
    uint32_t reg_scale_ctrl;
    uint32_t qts[2][0x10];
} JpegState;

typedef struct JpegHeader {
    uint8_t soi_marker[2];
    struct {
        uint8_t marker[2];
        uint16_t size;
        struct {
            uint8_t info;
            uint8_t data[0x40];
        } QEMU_PACKED qts[2];
    } QEMU_PACKED dqt;
    struct {
        uint8_t marker[2];
        uint16_t size;
        uint8_t precision;
        uint16_t height;
        uint16_t width;
        uint8_t num_components;
        struct {
            uint8_t id;
            uint8_t sampling;
            uint8_t qt;
        } QEMU_PACKED components[3];
    } QEMU_PACKED sof;
    struct {
        uint8_t marker[2];
        uint16_t size;
        uint8_t num_components;
        struct {
            uint8_t id;
            uint8_t ht;
        } QEMU_PACKED components[3];
        uint8_t spectral_start;
        uint8_t spectral_end;
        uint8_t ah_al;
    } QEMU_PACKED sos;
} QEMU_PACKED JpegHeader;
QEMU_BUILD_BUG_ON(sizeof(JpegHeader) != 0xa9);

#define JPEG_SEG(n, m) .marker={0xFF,(m)}, .size=cpu_to_be16(sizeof(((JpegHeader){}).n)-2)

static void jpeg_decompress422(void *src, unsigned long src_size, hwaddr dst, int dst_stride, unsigned int scale)
{
    unsigned int width, height, comp, row, x, y;
    const unsigned int num_rows = DCTSIZE / scale;

    struct jpeg_decompress_struct cinfo;
    struct jpeg_error_mgr jerr;
    JSAMPARRAY *data;
    uint32_t *buffer;

    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, src, src_size);
    jpeg_read_header(&cinfo, true);

    assert(cinfo.num_components == 3);
    assert(cinfo.comp_info[0].h_samp_factor == 2);
    assert(cinfo.comp_info[0].v_samp_factor == 1);
    assert(cinfo.comp_info[1].h_samp_factor == 1);
    assert(cinfo.comp_info[1].v_samp_factor == 1);
    assert(cinfo.comp_info[2].h_samp_factor == 1);
    assert(cinfo.comp_info[2].v_samp_factor == 1);

    width = cinfo.comp_info[0].width_in_blocks * num_rows;
    height = cinfo.comp_info[0].height_in_blocks * num_rows;

    cinfo.out_color_space = JCS_YCbCr;
    cinfo.scale_num = 1;
    cinfo.scale_denom = scale;
    cinfo.raw_data_out = true;
    jpeg_start_decompress(&cinfo);

    data = g_new(JSAMPARRAY, cinfo.num_components);
    for (comp = 0; comp < cinfo.num_components; comp++) {
        data[comp] = g_new(JSAMPROW, num_rows);
        for (row = 0; row < num_rows; row++) {
            data[comp][row] = g_new(JSAMPLE, width / (comp ? 2 : 1));
        }
    }
    buffer = g_new(uint32_t, width / 2);

    for (y = 0; y < height; y += num_rows) {
        jpeg_read_raw_data(&cinfo, data, num_rows);
        for (row = 0; row < num_rows; row++) {
            for (x = 0; x < width / 2; x++) {
                buffer[x] = (data[0][row][x*2+1] << 24) | (data[2][row][x] << 16) | (data[0][row][x*2] << 8) | data[1][row][x];
            }
            cpu_physical_memory_write(dst, buffer, width / 2 * sizeof(uint32_t));
            dst += dst_stride;
        }
    }

    g_free(buffer);
    for (comp = 0; comp < cinfo.num_components; comp++) {
        for (row = 0; row < num_rows; row++) {
            g_free(data[comp][row]);
        }
        g_free(data[comp]);
    }
    g_free(data);

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
}

static void jpeg_decompress(JpegState *s, JpegChannel *src, JpegChannel *dst)
{
    unsigned int i, j;
    uint8_t scale = 1 << (((s->reg_scale_ctrl >> 16) & 0xf) >> 1);
    uint16_t width = ((s->reg_jpeg_width & 0x1ff) << 4) * scale;
    uint16_t height = ((s->reg_jpeg_size & 0xffffff) << 5) / width;
    uint8_t offset = s->reg_jpeg_offset & 0x7f;
    size_t buffer_size = sizeof(JpegHeader) + src->num_cpy - offset;

    void *buffer = g_malloc(buffer_size);
    JpegHeader *header = buffer;

    if (s->reg_ctrl & ((1 << 18) | (1 << 16))) {
        hw_error("%s: only 4:2:2 jpegs are supported\n", __func__);
    }
    if (s->reg_size_ctrl & 1) {
        hw_error("%s: zoom not supported\n", __func__);
    }
    if (s->reg_size_ctrl & 2) {
        hw_error("%s: width not supported\n", __func__);
    }

    *header = (JpegHeader) {
        .soi_marker = {0xFF, 0xD8},
        .dqt = {
            JPEG_SEG(dqt, 0xDB),
            .qts = {{0}, {1}},
        },
        .sof = {
            JPEG_SEG(sof, 0xC0),
            .precision = 8,
            .height = cpu_to_be16(height),
            .width = cpu_to_be16(width),
            .num_components = 3,
            .components = {
                {1, 0x21, 0},
                {2, 0x11, 1},
                {3, 0x11, 1},
            },
        },
        .sos = {
            JPEG_SEG(sos, 0xDA),
            .num_components = 3,
            .components = {
                {1, 0},
                {2, 0x11},
                {3, 0x11},
            },
            .spectral_end = 0x3f,
        },
    };
    for (i = 0; i < 2; i++) {
        for (j = 0; j < 0x10; j++) {
            stl_he_p(&header->dqt.qts[i].data[4 * j], bswap32(s->qts[i][j]));
        }
    }

    cpu_physical_memory_read(s->mem_base + src->addr + offset, buffer + sizeof(JpegHeader), buffer_size - sizeof(JpegHeader));
    jpeg_decompress422(buffer, buffer_size, s->mem_base + dst->addr, dst->num_cpy + dst->num_skip, scale);

    g_free(buffer);
}

static void jpeg_fill(JpegState *s, JpegChannel *ch)
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

static void jpeg_update_irq(JpegState *s)
{
    qemu_set_irq(s->irq, !!(s->reg_inten & s->reg_intsts));
}

static void jpeg_command(JpegState *s)
{
    unsigned int i;
    int ch_en = 0;
    for (i = 0; i < NUM_CHANNELS; i++) {
        if (s->channels[i].ctrl & 1) {
            ch_en |= (1 << i);
        }
    }

    if (ch_en == 2 && s->channels[1].ctrl == 0x21) {
        jpeg_fill(s, &s->channels[1]);
    } else if (ch_en == 3) {
        jpeg_decompress(s, &s->channels[0], &s->channels[1]);
    } else {
        hw_error("%s: Unsupported command\n", __func__);
    }

    for (i = 0; i < NUM_CHANNELS; i++) {
        if (s->channels[i].ctrl & 1) {
            s->reg_intsts |= 1 << (i * 4);
            s->channels[i].ctrl &= ~1;
        }
    }
    jpeg_update_irq(s);
}

static uint64_t jpeg_ch_read(JpegState *s, unsigned int ch, hwaddr offset, unsigned size)
{
    JpegChannel *channel = &s->channels[ch];

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

static void jpeg_ch_write(JpegState *s, unsigned int ch, hwaddr offset, uint64_t value, unsigned size)
{
    JpegChannel *channel = &s->channels[ch];

    switch (offset) {
        case 0x00:
            channel->ctrl = value;
            if (ch == 1 && (value & 1)) {
                jpeg_command(s);
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

static uint64_t jpeg_read(void *opaque, hwaddr offset, unsigned size)
{
    JpegState *s = BIONZ_JPEG(opaque);

    if (offset >= 0x200 && offset < (0x200 + NUM_CHANNELS * 0x80)) {
        return jpeg_ch_read(s, (offset - 0x200) >> 7, offset & 0x7f, size);
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

static void jpeg_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    JpegState *s = BIONZ_JPEG(opaque);

    if (offset >= 0x200 && offset < (0x200 + NUM_CHANNELS * 0x80)) {
        jpeg_ch_write(s, (offset - 0x200) >> 7, offset & 0x7f, value, size);
    } else {
        switch (offset) {
            case 0:
                s->reg_intsts &= ~value;
                jpeg_update_irq(s);
                break;

            case 8:
                s->reg_inten = value;
                jpeg_update_irq(s);
                break;

            default:
                qemu_log_mask(LOG_UNIMP, "%s: unimplemented write @ 0x%" HWADDR_PRIx ": 0x%" PRIx64 "\n", __func__, offset, value);
        }
    }
}

static uint64_t jpeg_ctrl_read(void *opaque, hwaddr offset, unsigned size)
{
    JpegState *s = BIONZ_JPEG(opaque);

    if (offset >= 0x6c && offset < 0xac) {
        return s->qts[0][(offset - 0x6c) >> 2];
    } else if (offset >= 0xac && offset < 0xec) {
        return s->qts[1][(offset - 0xac) >> 2];
    } else {
        switch (offset) {
            case 0x00:
                return s->reg_ctrl;

            case 0x04:
                return s->reg_jpeg_offset;

            case 0x08:
                return s->reg_jpeg_size;

            case 0x24:
                return s->reg_jpeg_width;

            case 0x30:
                return s->reg_size_ctrl;

            case 0x50:
                return s->reg_scale_ctrl;

            default:
                qemu_log_mask(LOG_UNIMP, "%s: unimplemented read @ 0x%" HWADDR_PRIx "\n", __func__, offset);
                return 0;
        }
    }
}

static void jpeg_ctrl_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    JpegState *s = BIONZ_JPEG(opaque);

    if (offset >= 0x6c && offset < 0xac) {
        s->qts[0][(offset - 0x6c) >> 2] = value;
    } else if (offset >= 0xac && offset < 0xec) {
        s->qts[1][(offset - 0xac) >> 2] = value;
    } else {
        switch (offset) {
            case 0x00:
                s->reg_ctrl = value;
                break;

            case 0x04:
                s->reg_jpeg_offset = value;
                break;

            case 0x08:
                s->reg_jpeg_size = value;
                break;

            case 0x24:
                s->reg_jpeg_width = value;
                break;

            case 0x30:
                s->reg_size_ctrl = value;
                break;

            case 0x50:
                s->reg_scale_ctrl = value;
                break;

            default:
                qemu_log_mask(LOG_UNIMP, "%s: unimplemented write @ 0x%" HWADDR_PRIx ": 0x%" PRIx64 "\n", __func__, offset, value);
        }
    }
}

static const struct MemoryRegionOps jpeg_mmio0_ops = {
    .read = jpeg_read,
    .write = jpeg_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static const struct MemoryRegionOps jpeg_mmio1_ops = {
    .read = jpeg_ctrl_read,
    .write = jpeg_ctrl_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void jpeg_reset(DeviceState *dev)
{
    int i, j;
    JpegState *s = BIONZ_JPEG(dev);

    s->reg_intsts = 0;
    s->reg_inten = 0;

    s->reg_ctrl = 0;
    s->reg_jpeg_offset = 0;
    s->reg_jpeg_size = 0;
    s->reg_jpeg_width = 0;
    s->reg_size_ctrl = 0;
    s->reg_scale_ctrl = 0;

    for (i = 0; i < 2; i++) {
        for (j = 0; j < 0x10; j++) {
            s->qts[i][j] = 0;
        }
    }

    for (i = 0; i < NUM_CHANNELS; i++) {
        s->channels[i].ctrl = 0;
        s->channels[i].data = 0;
        s->channels[i].addr = 0;
        s->channels[i].num_cpy = 0;
        s->channels[i].num_skip = 0;
        s->channels[i].num_repeat = 0;
    }
}

static void jpeg_realize(DeviceState *dev, Error **errp)
{
    JpegState *s = BIONZ_JPEG(dev);

    memory_region_init_io(&s->mmio[0], OBJECT(dev), &jpeg_mmio0_ops, s, TYPE_BIONZ_JPEG ".mmio0", 0x800);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio[0]);

    memory_region_init_io(&s->mmio[1], OBJECT(dev), &jpeg_mmio1_ops, s, TYPE_BIONZ_JPEG ".mmio1", 0x800);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio[1]);

    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}

static Property jpeg_properties[] = {
    DEFINE_PROP_UINT32("base", JpegState, mem_base, 0),
    DEFINE_PROP_END_OF_LIST(),
};

static void jpeg_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = jpeg_realize;
    dc->reset = jpeg_reset;
    device_class_set_props(dc, jpeg_properties);
}

static const TypeInfo jpeg_info = {
    .name          = TYPE_BIONZ_JPEG,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(JpegState),
    .class_init    = jpeg_class_init,
};

static void jpeg_register_type(void)
{
    type_register_static(&jpeg_info);
}

type_init(jpeg_register_type)
