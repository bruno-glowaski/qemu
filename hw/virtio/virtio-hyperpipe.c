#include "qemu/osdep.h"
#include "hw/core/qdev.h"
#include "hw/virtio/vhost.h"
#include "hw/virtio/virtio.h"
#include "hw/virtio/virtio-pci.h"
#include "standard-headers/linux/virtio_ids.h"

#define TYPE_VIRTIO_HYPERPIPE "virtio-hyperpipe"
#define TYPE_VIRTIO_HYPERPIPE_PCI "virtio-hyperpipe-pci"
#define VIRTIO_HYPERPIPE(obj)                                                  \
  OBJECT_CHECK(VirtIOHyperpipe, obj, TYPE_VIRTIO_HYPERPIPE)
#define VIRTIO_HYPERPIPE_PCI(obj)                                              \
  OBJECT_CHECK(VirtIOHyperpipePCI, obj, TYPE_VIRTIO_HYPERPIPE_PCI)

struct virtio_hyperpipe_config {
  uint32_t queue_size;

} virtio_hyperpipe_config;

typedef struct VirtIOHyperpipe {
  VirtIODevice parent_obj;
  VirtQueue *queue;

  struct virtio_hyperpipe_config config;
} VirtIOHyperpipe;

typedef struct VirtIOHyperpipePCI {
  VirtIOPCIProxy parent_obj;
  VirtIOHyperpipe vdev;
} VirtIOHyperpipePCI;

static const VMStateDescription vmstate_virtio_hyperpipe = {
    .name = "virtio-hyperpipe",
    .minimum_version_id = 1,
    .version_id = 1,
    .fields = (VMStateField[]){VMSTATE_VIRTIO_DEVICE, VMSTATE_END_OF_LIST()},
};

static const Property virtio_hyperpipe_properties[] = {
    DEFINE_PROP_UINT32("queue_size", VirtIOHyperpipe, config.queue_size, 256)};

static void virtio_hyperpipe_queue_handler(VirtIODevice *vdev, VirtQueue *vq) {}

static void virtio_hyperpipe_realize(DeviceState *dev, Error **errp) {
  VirtIODevice *vdev = VIRTIO_DEVICE(dev);
  VirtIOHyperpipe *vhp = VIRTIO_HYPERPIPE(dev);

  virtio_init(vdev, VIRTIO_ID_HYPERPIPE,
              sizeof(struct virtio_hyperpipe_config));

  vhp->queue = virtio_add_queue(vdev, vhp->config.queue_size,
                                virtio_hyperpipe_queue_handler);
}

static void virtio_hyperpipe_device_unrealize(DeviceState *dev) {
  VirtIODevice *vdev = VIRTIO_DEVICE(dev);

  virtio_del_queue(vdev, 0);
  virtio_cleanup(vdev);
}

static uint64_t virtio_hyperpipe_get_features(VirtIODevice *vdev,
                                              uint64_t requested_features,
                                              Error **errp) {
  return requested_features;
}

static void virtio_hyperpipe_instance_init(Object *obj) {}

static void virtio_hyperpipe_class_init(ObjectClass *klass, const void *data) {
  DeviceClass *dc = DEVICE_CLASS(klass);
  VirtioDeviceClass *vdc = VIRTIO_DEVICE_CLASS(klass);

  dc->vmsd = &vmstate_virtio_hyperpipe;
  device_class_set_props(dc, virtio_hyperpipe_properties);
  set_bit(DEVICE_CATEGORY_MISC, dc->categories);
  vdc->realize = virtio_hyperpipe_realize;
  vdc->unrealize = virtio_hyperpipe_device_unrealize;
  vdc->get_features = virtio_hyperpipe_get_features;
}

static const TypeInfo virtio_hyperpipe_info = {
    .name = TYPE_VIRTIO_HYPERPIPE,
    .parent = TYPE_VIRTIO_DEVICE,
    .instance_size = sizeof(VirtIOHyperpipe),
    .instance_init = virtio_hyperpipe_instance_init,
    .class_init = virtio_hyperpipe_class_init,
};

static void virtio_hyperpipe_pci_realize(VirtIOPCIProxy *vpci_dev,
                                         Error **errp) {
  VirtIOHyperpipePCI *dev =
      container_of(vpci_dev, VirtIOHyperpipePCI, parent_obj);
  DeviceState *vdev = DEVICE(&dev->vdev);

  virtio_pci_force_virtio_1(vpci_dev);
  qdev_realize(vdev, BUS(&vpci_dev->bus), errp);
}

static void virtio_hyperpipe_pci_instance_init(Object *obj) {
  VirtIOHyperpipePCI *dev = VIRTIO_HYPERPIPE_PCI(obj);

  virtio_instance_init_common(obj, &dev->vdev, sizeof(dev->vdev),
                              TYPE_VIRTIO_HYPERPIPE);
}

static void virtio_hyperpipe_pci_class_init(ObjectClass *klass,
                                            const void *data) {
  DeviceClass *dc = DEVICE_CLASS(klass);
  VirtioPCIClass *k = VIRTIO_PCI_CLASS(klass);
  PCIDeviceClass *pcidev_k = PCI_DEVICE_CLASS(klass);
  k->realize = virtio_hyperpipe_pci_realize;
  set_bit(DEVICE_CATEGORY_MISC, dc->categories);
  pcidev_k->revision = VIRTIO_PCI_ABI_VERSION;
  pcidev_k->class_id = PCI_CLASS_OTHERS;
  dc->hotpluggable = false;
}

static const TypeInfo virtio_hyperpipe_pci_info = {
    .name = TYPE_VIRTIO_HYPERPIPE_PCI,
    .parent = TYPE_VIRTIO_PCI,
    .instance_size = sizeof(VirtIOHyperpipePCI),
    .class_init = virtio_hyperpipe_pci_class_init,
    .instance_init = virtio_hyperpipe_pci_instance_init,
    .interfaces = (InterfaceInfo[]){{INTERFACE_PCIE_DEVICE}, {}}};

static void virtio_hyperpipe_type_init(void) {
  type_register_static(&virtio_hyperpipe_info);
  type_register_static(&virtio_hyperpipe_pci_info);
}

type_init(virtio_hyperpipe_type_init)
