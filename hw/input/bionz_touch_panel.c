/* QEMU model of the Sony CXD4108 touch panel */

#include "qemu/osdep.h"
#include "hw/adc/analog.h"
#include "hw/qdev-properties.h"
#include "sysemu/sysemu.h"
#include "ui/console.h"
#include "ui/input.h"

#define MAX_VALUE 255
#define XMIN 16
#define XMAX 244
#define YMIN 23
#define YMAX 241

#define DELAY_MS 180

#define TYPE_BIONZ_TOUCH_PANEL "bionz_touch_panel"
#define BIONZ_TOUCH_PANEL(obj) OBJECT_CHECK(TouchPanelState, (obj), TYPE_BIONZ_TOUCH_PANEL)

typedef struct TouchState {
    int buttons;
    int x;
    int y;
} TouchState;

typedef struct TouchEvent {
    TouchState state;
    QSIMPLEQ_ENTRY(TouchEvent) next;
} TouchEvent;

typedef struct TouchPanelState {
    DeviceState parent_obj;
    QSIMPLEQ_HEAD(, TouchEvent) event_queue;
    QEMUTimer *timer;

    uint8_t channels[2];

    bool sel[2];
    TouchState state;
    int buttons_last;
} TouchPanelState;

static void touch_panel_update(TouchPanelState *s)
{
    AnalogBus *bus = ANALOG_BUS(qdev_get_parent_bus(DEVICE(s)));
    unsigned int x = MAX_VALUE, y = MAX_VALUE;

    if (s->state.buttons) {
        switch ((s->sel[1] << 1) | s->sel[0]) {
            case 0b01:
                x = XMIN + s->state.x * (XMAX - XMIN) / INPUT_EVENT_ABS_MAX;
                break;

            case 0b10:
                y = YMIN + s->state.y * (YMAX - YMIN) / INPUT_EVENT_ABS_MAX;
                break;

            case 0b11:
                x = 0;
                break;
        }
    }

    analog_bus_set(bus, s->channels[0], x, MAX_VALUE);
    analog_bus_set(bus, s->channels[1], y, MAX_VALUE);
}

static void touch_panel_gpio_handler(void *opaque, int irq, int level)
{
    TouchPanelState *s = BIONZ_TOUCH_PANEL(opaque);
    assert(irq < 2);
    s->sel[irq] = !!level;
    touch_panel_update(s);
}

static void touch_panel_fire(TouchPanelState *s)
{
    TouchEvent *e = QSIMPLEQ_FIRST(&s->event_queue);
    s->state = e->state;
    touch_panel_update(s);
    timer_mod(s->timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + DELAY_MS);
}

static void touch_panel_tick(void *opaque)
{
    TouchPanelState *s = BIONZ_TOUCH_PANEL(opaque);
    TouchEvent *e = QSIMPLEQ_FIRST(&s->event_queue);
    QSIMPLEQ_REMOVE_HEAD(&s->event_queue, next);
    g_free(e);

    if (!QSIMPLEQ_EMPTY(&s->event_queue)) {
        touch_panel_fire(s);
    }
}

static void touch_panel_mouse_event(void *opaque, int x, int y, int z, int buttons_state)
{
    TouchPanelState *s = BIONZ_TOUCH_PANEL(opaque);
    TouchEvent *e;
    bool first = QSIMPLEQ_EMPTY(&s->event_queue);

    if (buttons_state != s->buttons_last) {
        e = g_new0(TouchEvent, 1);
        e->state = (TouchState) {buttons_state, x, y};
        QSIMPLEQ_INSERT_TAIL(&s->event_queue, e, next);
        if (first) {
            touch_panel_fire(s);
        }
        s->buttons_last = buttons_state;
    }
}

static void touch_panel_reset(DeviceState *dev)
{
    TouchPanelState *s = BIONZ_TOUCH_PANEL(dev);
    timer_del(s->timer);
    s->sel[0] = 1;
    s->sel[1] = 1;
    s->state = (TouchState) {0, 0, 0};
    s->buttons_last = 0;
}

static void touch_panel_realize(DeviceState *dev, Error **errp)
{
    TouchPanelState *s = BIONZ_TOUCH_PANEL(dev);
    QSIMPLEQ_INIT(&s->event_queue);
    s->timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, touch_panel_tick, s);

    qemu_add_mouse_event_handler(touch_panel_mouse_event, s, 1, "touch_panel");
    qdev_init_gpio_in(dev, touch_panel_gpio_handler, 2);
}

static Property touch_panel_properties[] = {
    DEFINE_PROP_UINT8("channel_x", TouchPanelState, channels[0], 6),
    DEFINE_PROP_UINT8("channel_y", TouchPanelState, channels[1], 7),
    DEFINE_PROP_END_OF_LIST(),
};

static void touch_panel_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->bus_type = TYPE_ANALOG_BUS;
    dc->realize = touch_panel_realize;
    device_class_set_props(dc, touch_panel_properties);
    dc->reset = touch_panel_reset;
}

static const TypeInfo touch_panel_info = {
    .name          = TYPE_BIONZ_TOUCH_PANEL,
    .parent        = TYPE_DEVICE,
    .instance_size = sizeof(TouchPanelState),
    .class_init    = touch_panel_class_init,
};

static void touch_panel_register_type(void)
{
    type_register_static(&touch_panel_info);
}

type_init(touch_panel_register_type)
