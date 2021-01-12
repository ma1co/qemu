/* QEMU model of configurable buttons that connect to an ADC */

#include "qemu/osdep.h"
#include "hw/adc/analog.h"
#include "hw/qdev-properties.h"
#include "sysemu/sysemu.h"
#include "ui/console.h"
#include "ui/input.h"
#include "ui/keymaps.h"

#define NUM_CHANNELS 2

#define MAX_VALUE 255
#define R1 10000
#define R2 2200

#define DELAY_MS 180

#define TYPE_BIONZ_BUTTONS "bionz_buttons"
#define BIONZ_BUTTONS(obj) OBJECT_CHECK(ButtonsState, (obj), TYPE_BIONZ_BUTTONS)

typedef struct KeyState {
    uint8_t active  : 1;
    uint8_t channel : 3;
    uint8_t button  : 4;
} KeyState;

typedef struct KeyEvent {
    KeyState state;
    QSIMPLEQ_ENTRY(KeyEvent) next;
} KeyEvent;

typedef struct ButtonsState {
    DeviceState parent_obj;
    QSIMPLEQ_HEAD(, KeyEvent) event_queue;
    QEMUTimer *timer;

    uint8_t channels[NUM_CHANNELS];
    char *keys[NUM_CHANNELS];

    KeyState keymap[Q_KEY_CODE__MAX];
    KeyState state;
} ButtonsState;

static const int keymap[256][2] = {
    ['d'] = {Q_KEY_CODE_DOWN, Q_KEY_CODE_KP_2},  // down
    ['h'] = {Q_KEY_CODE_H},                      // home
    ['l'] = {Q_KEY_CODE_LEFT, Q_KEY_CODE_KP_4},  // left
    ['m'] = {Q_KEY_CODE_M},                      // menu
    ['r'] = {Q_KEY_CODE_RIGHT, Q_KEY_CODE_KP_6}, // right
    ['s'] = {Q_KEY_CODE_RET},                    // set
    ['t'] = {Q_KEY_CODE_T},                      // tele
    ['u'] = {Q_KEY_CODE_UP, Q_KEY_CODE_KP_8},    // up
    ['w'] = {Q_KEY_CODE_W},                      // wide
};

static void buttons_update(ButtonsState *s)
{
    AnalogBus *bus = ANALOG_BUS(qdev_get_parent_bus(DEVICE(s)));
    unsigned int value;
    int i;

    for (i = 0; i < NUM_CHANNELS; i++) {
        value = MAX_VALUE;
        if (i == s->state.channel && s->state.active) {
            value = MAX_VALUE * (R2 * s->state.button) / (R1 + R2 * s->state.button);
        }
        analog_bus_set(bus, s->channels[i], value, MAX_VALUE);
    }
}

static void buttons_fire(ButtonsState *s)
{
    KeyEvent *e = QSIMPLEQ_FIRST(&s->event_queue);
    s->state = e->state;
    buttons_update(s);
    timer_mod(s->timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + DELAY_MS);
}

static void buttons_tick(void *opaque)
{
    ButtonsState *s = BIONZ_BUTTONS(opaque);
    KeyEvent *e = QSIMPLEQ_FIRST(&s->event_queue);
    QSIMPLEQ_REMOVE_HEAD(&s->event_queue, next);
    g_free(e);

    if (!QSIMPLEQ_EMPTY(&s->event_queue)) {
        buttons_fire(s);
    }
}

static void buttons_kbd_event(void *opaque, int keycode)
{
    ButtonsState *s = BIONZ_BUTTONS(opaque);
    KeyEvent *e;
    bool first = QSIMPLEQ_EMPTY(&s->event_queue);
    KeyState state = s->keymap[qemu_input_key_number_to_qcode(keycode & SCANCODE_KEYCODEMASK)];

    if (!state.active) {
        return;
    }
    state.active = !(keycode & SCANCODE_UP);

    e = g_new0(KeyEvent, 1);
    e->state = state;
    QSIMPLEQ_INSERT_TAIL(&s->event_queue, e, next);
    if (first) {
        buttons_fire(s);
    }
}

static void buttons_reset(DeviceState *dev)
{
    ButtonsState *s = BIONZ_BUTTONS(dev);
    timer_del(s->timer);
    s->state = (KeyState) {0, 0, 0};
}

static void buttons_realize(DeviceState *dev, Error **errp)
{
    ButtonsState *s = BIONZ_BUTTONS(dev);
    int i, j, k, code;

    QSIMPLEQ_INIT(&s->event_queue);
    s->timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, buttons_tick, s);

    qemu_add_kbd_event_handler(buttons_kbd_event, s);

    memset(s->keymap, 0, sizeof(s->keymap));
    for (i = 0; i < NUM_CHANNELS; i++) {
        for (j = 0; s->keys[i] && s->keys[i][j]; j++) {
            for (k = 0; k < ARRAY_SIZE(keymap[0]); k++) {
                code = keymap[(int)s->keys[i][j]][k];
                if (code) {
                    s->keymap[code] = (KeyState) {1, i, j};
                }
            }
        }
    }
}

static Property buttons_properties[] = {
    DEFINE_PROP_UINT8("channel0", ButtonsState, channels[0], 2),
    DEFINE_PROP_UINT8("channel1", ButtonsState, channels[1], 3),
    DEFINE_PROP_STRING("keys0", ButtonsState, keys[0]),
    DEFINE_PROP_STRING("keys1", ButtonsState, keys[1]),
    DEFINE_PROP_END_OF_LIST(),
};

static void buttons_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->bus_type = TYPE_ANALOG_BUS;
    dc->realize = buttons_realize;
    device_class_set_props(dc, buttons_properties);
    dc->reset = buttons_reset;
}

static const TypeInfo buttons_info = {
    .name          = TYPE_BIONZ_BUTTONS,
    .parent        = TYPE_DEVICE,
    .instance_size = sizeof(ButtonsState),
    .class_init    = buttons_class_init,
};

static void buttons_register_type(void)
{
    type_register_static(&buttons_info);
}

type_init(buttons_register_type)
