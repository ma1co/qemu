/* QEMU model of a Sony CXD4108 peripheral needed by the cam_sync task  */

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "qemu/log.h"

#define TYPE_BIONZ_CAM_SYNC "bionz_cam_sync"
#define BIONZ_CAM_SYNC(obj) OBJECT_CHECK(CamSyncState, (obj), TYPE_BIONZ_CAM_SYNC)

typedef struct CamSyncState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    uint32_t value;
} CamSyncState;

static uint64_t cam_sync_read(void *opaque, hwaddr offset, unsigned size)
{
    CamSyncState *s = BIONZ_CAM_SYNC(opaque);

    switch (offset) {
        case 0:
            return s->value;

        default:
            qemu_log_mask(LOG_UNIMP, "%s: unimplemented read @ 0x%" HWADDR_PRIx "\n", __func__, offset);
            return 0;
    }
}

static void cam_sync_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    CamSyncState *s = BIONZ_CAM_SYNC(opaque);

    switch (offset) {
        case 0:
            if (value & 1) {
                s->value |= 3;
            } else {
                s->value &= ~3;
            }
            break;

        default:
            qemu_log_mask(LOG_UNIMP, "%s: unimplemented write @ 0x%" HWADDR_PRIx ": 0x%" PRIx64 "\n", __func__, offset, value);
    }
}

static const struct MemoryRegionOps cam_sync_ops = {
    .read = cam_sync_read,
    .write = cam_sync_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void cam_sync_reset(DeviceState *dev)
{
    CamSyncState *s = BIONZ_CAM_SYNC(dev);
    s->value = 0x2020004;
}

static void cam_sync_realize(DeviceState *dev, Error **errp)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    CamSyncState *s = BIONZ_CAM_SYNC(dev);
    memory_region_init_io(&s->mmio, OBJECT(dev), &cam_sync_ops, s, TYPE_BIONZ_CAM_SYNC, 0x10);
    sysbus_init_mmio(sbd, &s->mmio);
}

static void cam_sync_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = cam_sync_realize;
    dc->reset = cam_sync_reset;
}

static const TypeInfo cam_sync_info = {
    .name          = TYPE_BIONZ_CAM_SYNC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(CamSyncState),
    .class_init    = cam_sync_class_init,
};

static void cam_sync_register_type(void)
{
    type_register_static(&cam_sync_info);
}

type_init(cam_sync_register_type)
