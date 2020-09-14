/* Similar to the generic loader, but adds a read-only memory region */
#include "qemu/osdep.h"
#include "hw/qdev-properties.h"
#include "hw/sysbus.h"
#include "qapi/error.h"
#include "sysemu/dma.h"

#define TYPE_GENERIC_ROM "rom-loader"
#define GENERIC_ROM(obj) OBJECT_CHECK(GenericRomState, (obj), TYPE_GENERIC_ROM)

typedef struct GenericRomState {
    DeviceState parent_obj;
    char *name;
    uint64_t addr;
    uint64_t data;
    uint8_t data_len;
} GenericRomState;

static void generic_rom_realize(DeviceState *dev, Error **errp)
{
    GenericRomState *s = GENERIC_ROM(dev);

    if (!s->addr || !s->data_len || s->data_len > 8) {
        error_setg(errp, "please include valid arguments");
        return;
    }

    MemoryRegion *mem = g_new(MemoryRegion, 1);
    memory_region_init_ram_ptr(mem, NULL, s->name, s->data_len, &s->data);
    memory_region_set_readonly(mem, true);
    memory_region_add_subregion(get_system_memory(), s->addr, mem);
}

static Property generic_rom_props[] = {
    DEFINE_PROP_STRING("name", GenericRomState, name),
    DEFINE_PROP_UINT64("addr", GenericRomState, addr, 0),
    DEFINE_PROP_UINT64("data", GenericRomState, data, 0),
    DEFINE_PROP_UINT8("data-len", GenericRomState, data_len, 0),
    DEFINE_PROP_END_OF_LIST(),
};

static void generic_rom_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = generic_rom_realize;
    device_class_set_props(dc, generic_rom_props);
}

static TypeInfo generic_rom_info = {
    .name = TYPE_GENERIC_ROM,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(GenericRomState),
    .class_init = generic_rom_class_init,
};

static void generic_rom_register_type(void)
{
    type_register_static(&generic_rom_info);
}

type_init(generic_rom_register_type)
