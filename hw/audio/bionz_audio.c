/* QEMU model of the Sony CXD4108 MPEG-1 audio layer II decoder / audio peripheral */

#include "qemu/osdep.h"
#include "audio/audio.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/sysbus.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "sysemu/sysemu.h"
#include <mad.h>
#include "sf_table.h"

#define DELAY_MS 100

#define SAMPLE_RATE 32000

#define NUM_SB 32
#define NUM_GR 12
#define NUM_SF 3

#define SAMPLES_PER_FRAME (NUM_GR * NUM_SF * NUM_SB)
#define BYTES_PER_FRAME (NUM_SF * NUM_SB + SAMPLES_PER_FRAME)

#define TYPE_BIONZ_AUDIO "bionz_audio"
#define BIONZ_AUDIO(obj) OBJECT_CHECK(AudioState, (obj), TYPE_BIONZ_AUDIO)

typedef struct AudioState {
    SysBusDevice parent_obj;
    MemoryRegion mmio[2];
    qemu_irq irq;
    QEMUTimer *timer;
    QEMUSoundCard card;
    SWVoiceOut *voice;

    uint32_t mem_base;
    struct mad_frame frame;
    struct mad_synth synth;

    uint16_t reg_ctrl;
    uint32_t reg_intsts;
    uint32_t reg_inten;
    uint32_t reg_ch_conf;
    uint32_t reg_ch_stat;
    uint32_t reg_ch_curr;
    uint32_t reg_ch_addr;
    uint32_t reg_ch_size;
} AudioState;

static void audio_update_irq(AudioState *s)
{
    qemu_set_irq(s->irq, !!(s->reg_inten & s->reg_intsts));
}

static void frame_read(void *data, struct mad_frame *frame)
{
    unsigned int i, sb;
    uint8_t scalefactor[NUM_SB][NUM_SF];
    int8_t sample;

    for (i = 0; i < NUM_SF; i++) {
        for (sb = 0; sb < NUM_SB; sb++) {
            scalefactor[sb][i] = *(uint8_t *) data++;
        }
    }

    for (i = 0; i < NUM_GR * NUM_SF; i++) {
        for (sb = 0; sb < NUM_SB; sb++) {
            sample = *(int8_t *) data++;
            frame->sbsample[0][i][sb] = mad_f_mul(sample << (MAD_F_FRACBITS - 7), sf_table[scalefactor[sb][i / NUM_GR]]);
        }
    }
}

static void frame_write_samples(struct mad_pcm *pcm, int16_t *samples)
{
    unsigned int i;
    mad_fixed_t sample;

    assert(pcm->length == SAMPLES_PER_FRAME);
    for (i = 0; i < SAMPLES_PER_FRAME; i++) {
        sample = pcm->samples[0][i];
        sample += (1L << (MAD_F_FRACBITS - 16));
        if (sample >= MAD_F_ONE) {
            sample = MAD_F_ONE - 1;
        } else if (sample < -MAD_F_ONE) {
            sample = -MAD_F_ONE;
        }
        samples[i] = sample >> (MAD_F_FRACBITS + 1 - 16);
    }
}

static void frame_decode(AudioState *s, void *data, int16_t *samples)
{
    frame_read(data, &s->frame);
    mad_synth_frame(&s->synth, &s->frame);
    frame_write_samples(&s->synth.pcm, samples);
}

static void frame_decode_next(AudioState *s, int16_t *samples)
{
    uint8_t buffer[BYTES_PER_FRAME];

    cpu_physical_memory_read(s->mem_base + s->reg_ch_curr, buffer, BYTES_PER_FRAME);
    frame_decode(s, buffer, samples);

    s->reg_ch_curr += BYTES_PER_FRAME;
    if (s->reg_ch_curr >= s->reg_ch_addr + s->reg_ch_size) {
        s->reg_ch_curr = s->reg_ch_addr;
        if (!(s->reg_ch_stat & 1)) {
            s->reg_intsts |= 1;
            audio_update_irq(s);
        }
    }
}

static void audio_callback(void *opaque, int free)
{
    AudioState *s = BIONZ_AUDIO(opaque);
    int16_t samples[SAMPLES_PER_FRAME];

    if (!(s->reg_ctrl & 1)) {
        s->reg_intsts &= ~1;
        s->reg_ch_stat = 0;
        s->reg_ch_curr = 0;
        AUD_set_active_out(s->voice, 0);
        audio_update_irq(s);
        return;
    }

    if (free < sizeof(samples)) {
        return;
    }

    frame_decode_next(s, samples);
    AUD_write(s->voice, samples, sizeof(samples));
}

static void audio_start(void *opaque)
{
    AudioState *s = BIONZ_AUDIO(opaque);

    mad_synth_init(&s->synth);
    AUD_set_active_out(s->voice, 1);
}

static uint64_t audio_ctrl_read(void *opaque, hwaddr offset, unsigned size)
{
    AudioState *s = BIONZ_AUDIO(opaque);

    switch (offset) {
        case 0:
            return s->reg_ctrl;

        default:
            qemu_log_mask(LOG_UNIMP, "%s: unimplemented read @ 0x%" HWADDR_PRIx "\n", __func__, offset);
            return 0;
    }
}

static void audio_ctrl_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    AudioState *s = BIONZ_AUDIO(opaque);

    switch (offset) {
        case 0:
            s->reg_ctrl = value & ~0x2000;
            if (value & 0x2000) {
                s->reg_ch_stat = (1 << 31) | ((s->reg_ch_conf >> 4) & 1);
                s->reg_ch_curr = s->reg_ch_addr;
                s->reg_ctrl |= 1;
                timer_mod(s->timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + DELAY_MS);
            }
            break;

        default:
            qemu_log_mask(LOG_UNIMP, "%s: unimplemented write @ 0x%" HWADDR_PRIx ": 0x%" PRIx64 "\n", __func__, offset, value);
    }
}

static uint64_t audio_ch_read(void *opaque, hwaddr offset, unsigned size)
{
    AudioState *s = BIONZ_AUDIO(opaque);

    switch (offset) {
        case 0x000:
            return s->reg_intsts;

        case 0x008:
            return s->reg_inten;

        case 0x200:
            return s->reg_ch_conf;

        case 0x204:
            return s->reg_ch_stat | (s->reg_ch_curr & 0xffffff8);

        case 0x220:
            return s->reg_ch_addr;

        case 0x224:
            return s->reg_ch_size;

        default:
            qemu_log_mask(LOG_UNIMP, "%s: unimplemented read @ 0x%" HWADDR_PRIx "\n", __func__, offset);
            return 0;
    }
}

static void audio_ch_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    AudioState *s = BIONZ_AUDIO(opaque);

    switch (offset) {
        case 0x000:
            s->reg_intsts &= ~value;
            audio_update_irq(s);
            break;

        case 0x008:
            s->reg_inten = value;
            audio_update_irq(s);
            break;

        case 0x200:
            s->reg_ch_conf = value;
            break;

        case 0x204:
            s->reg_ch_stat = (s->reg_ch_stat & ~7) | (value & 7);
            break;

        case 0x220:
            s->reg_ch_addr = value;
            break;

        case 0x224:
            s->reg_ch_size = value;
            break;

        default:
            qemu_log_mask(LOG_UNIMP, "%s: unimplemented write @ 0x%" HWADDR_PRIx ": 0x%" PRIx64 "\n", __func__, offset, value);
    }
}

static const struct MemoryRegionOps audio_mmio0_ops = {
    .read = audio_ctrl_read,
    .write = audio_ctrl_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 2,
    .valid.max_access_size = 4,
};

static const struct MemoryRegionOps audio_mmio1_ops = {
    .read = audio_ch_read,
    .write = audio_ch_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void audio_reset(DeviceState *dev)
{
    AudioState *s = BIONZ_AUDIO(dev);

    timer_del(s->timer);

    s->reg_ctrl = 0;
    s->reg_intsts = 0;
    s->reg_inten = 0;
    s->reg_ch_conf = 0;
    s->reg_ch_stat = 0;
    s->reg_ch_curr = 0;
    s->reg_ch_addr = 0;
    s->reg_ch_size = 0;
}

static void audio_realize(DeviceState *dev, Error **errp)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    AudioState *s = BIONZ_AUDIO(dev);

    struct audsettings as = {SAMPLE_RATE, 1, AUDIO_FORMAT_S16, AUDIO_HOST_ENDIANNESS};
    AUD_register_card("bionz", &s->card);
    s->voice = AUD_open_out(&s->card, s->voice, "bionz", s, audio_callback, &as);
    if (!s->voice) {
        AUD_remove_card(&s->card);
        error_setg(errp, "Cannot open voice");
        return;
    }

    s->timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, audio_start, s);

    mad_frame_init(&s->frame);
    s->frame.header.layer = MAD_LAYER_II;
    s->frame.header.mode = MAD_MODE_SINGLE_CHANNEL;

    memory_region_init_io(&s->mmio[0], OBJECT(dev), &audio_mmio0_ops, s, TYPE_BIONZ_AUDIO ".mmio0", 0x1000);
    sysbus_init_mmio(sbd, &s->mmio[0]);

    memory_region_init_io(&s->mmio[1], OBJECT(dev), &audio_mmio1_ops, s, TYPE_BIONZ_AUDIO ".mmio1", 0x1000);
    sysbus_init_mmio(sbd, &s->mmio[1]);

    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}

static Property audio_properties[] = {
    DEFINE_AUDIO_PROPERTIES(AudioState, card),
    DEFINE_PROP_UINT32("base", AudioState, mem_base, 0),
    DEFINE_PROP_END_OF_LIST(),
};

static void audio_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = audio_realize;
    dc->reset = audio_reset;
    device_class_set_props(dc, audio_properties);
}

static const TypeInfo audio_info = {
    .name          = TYPE_BIONZ_AUDIO,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(AudioState),
    .class_init    = audio_class_init,
};

static void audio_register_type(void)
{
    type_register_static(&audio_info);
}

type_init(audio_register_type)
