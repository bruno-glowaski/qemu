#include "qemu/osdep.h"
#include "hw/virtio/vhost.h"
#include "hw/virtio/virtio.h"

#define TYPE_VIRTIO_HYPERPIPE "virtio-hyperpipe"
#define VIRTIO_HYPERPIPE OBJECT_CHECK(obj)

typedef struct VirtIOHyperpipe {
  VirtIODevice parent_obj;
} VirtIOHyperpipe;

static void virtio_hyperpipe_instance_init(Object *obj) {}

static void virtio_hyperpipe_class_init(ObjectClass *klass, const void *data) {
  DeviceClass *dc = DEVICE_CLASS(klass);

  set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo virtio_hyperpipe_info = {
    .name = TYPE_VIRTIO_HYPERPIPE,
    .parent = TYPE_VIRTIO_DEVICE,
    .instance_size = sizeof(VirtIOHyperpipe),
    .instance_init = virtio_hyperpipe_instance_init,
    .class_init = virtio_hyperpipe_class_init,
};

static void virtio_hyperpipe_type_init(void) {
  type_register_static(&virtio_hyperpipe_info);
}

type_init(virtio_hyperpipe_type_init)
