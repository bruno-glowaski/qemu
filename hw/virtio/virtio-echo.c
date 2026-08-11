#include "qemu/osdep.h"
#include "hw/core/qdev.h"
#include "hw/virtio/vhost.h"
#include "hw/virtio/virtio.h"
#include "hw/virtio/virtio-pci.h"
#include "standard-headers/linux/virtio_ids.h"
#include <stddef.h>

#define TYPE_VIRTIO_ECHO "virtio-echo"
#define TYPE_VIRTIO_ECHO_PCI "virtio-echo-pci"
#define VIRTIO_ECHO(obj) OBJECT_CHECK(VirtIOecho, obj, TYPE_VIRTIO_ECHO)
#define VIRTIO_ECHO_PCI(obj)                                                   \
  OBJECT_CHECK(VirtIOechoPCI, obj, TYPE_VIRTIO_ECHO_PCI)

struct virtio_echo_config {
  uint32_t queue_size;

} virtio_echo_config;

typedef struct VirtIOecho {
  VirtIODevice parent_obj;
  VirtQueue *queue;

  struct virtio_echo_config config;
} VirtIOecho;

typedef struct VirtIOechoPCI {
  VirtIOPCIProxy parent_obj;
  VirtIOecho vdev;
} VirtIOechoPCI;

static const VMStateDescription vmstate_virtio_echo = {
    .name = "virtio-echo",
    .minimum_version_id = 1,
    .version_id = 1,
    .fields = (VMStateField[]){VMSTATE_VIRTIO_DEVICE, VMSTATE_END_OF_LIST()},
};

static const Property virtio_echo_properties[] = {
    DEFINE_PROP_UINT32("queue_size", VirtIOecho, config.queue_size, 256)};

static void virtio_echo_queue_handler(VirtIODevice *vdev, VirtQueue *vq) {
  VirtQueueElement *elem;

  while ((elem = virtqueue_pop(vq, sizeof(VirtQueueElement)))) {
    for (unsigned int i = 0; i < elem->out_num; i++) {
      void *data = elem->out_sg[i].iov_base;
      size_t len = elem->out_sg[i].iov_len;
    }

    virtqueue_push(vq, elem, 0);
    g_free(elem);
  }
  virtio_notify(vdev, vq);
}

static void virtio_echo_realize(DeviceState *dev, Error **errp) {
  VirtIODevice *vdev = VIRTIO_DEVICE(dev);
  VirtIOecho *vhp = VIRTIO_ECHO(dev);

  virtio_init(vdev, VIRTIO_ID_ECHO, sizeof(struct virtio_echo_config));

  vhp->queue =
      virtio_add_queue(vdev, vhp->config.queue_size, virtio_echo_queue_handler);
}

static void virtio_echo_unrealize(DeviceState *dev) {
  VirtIODevice *vdev = VIRTIO_DEVICE(dev);

  virtio_del_queue(vdev, 0);
  virtio_cleanup(vdev);
}

static void virtio_echo_set_config(VirtIODevice *vdev, const uint8_t *config) {}

static uint64_t virtio_echo_get_features(VirtIODevice *vdev,
                                         uint64_t requested_features,
                                         Error **errp) {
  return requested_features;
}

static void virtio_echo_set_features(VirtIODevice *vdev, uint64_t features) {}

static uint64_t virtio_echo_bad_features(VirtIODevice *vdev) { return 0; }

static void virtio_echo_instance_init(Object *obj) {}

static void virtio_echo_class_init(ObjectClass *klass, const void *data) {
  DeviceClass *dc = DEVICE_CLASS(klass);
  VirtioDeviceClass *vdc = VIRTIO_DEVICE_CLASS(klass);

  dc->vmsd = &vmstate_virtio_echo;
  device_class_set_props(dc, virtio_echo_properties);
  set_bit(DEVICE_CATEGORY_MISC, dc->categories);
  vdc->realize = virtio_echo_realize;
  vdc->unrealize = virtio_echo_unrealize;
  vdc->set_config = virtio_echo_set_config;
  vdc->get_features = virtio_echo_get_features;
  vdc->set_features = virtio_echo_set_features;
  vdc->bad_features = virtio_echo_bad_features;
}

static const TypeInfo virtio_echo_info = {
    .name = TYPE_VIRTIO_ECHO,
    .parent = TYPE_VIRTIO_DEVICE,
    .instance_size = sizeof(VirtIOecho),
    .instance_init = virtio_echo_instance_init,
    .class_init = virtio_echo_class_init,
};

static void virtio_echo_pci_realize(VirtIOPCIProxy *vpci_dev, Error **errp) {
  VirtIOechoPCI *dev = container_of(vpci_dev, VirtIOechoPCI, parent_obj);
  DeviceState *vdev = DEVICE(&dev->vdev);

  virtio_pci_force_virtio_1(vpci_dev);
  qdev_realize(vdev, BUS(&vpci_dev->bus), errp);
}

static void virtio_echo_pci_instance_init(Object *obj) {
  VirtIOechoPCI *dev = VIRTIO_ECHO_PCI(obj);

  virtio_instance_init_common(obj, &dev->vdev, sizeof(dev->vdev),
                              TYPE_VIRTIO_ECHO);
}

static void virtio_echo_pci_class_init(ObjectClass *klass, const void *data) {
  DeviceClass *dc = DEVICE_CLASS(klass);
  VirtioPCIClass *k = VIRTIO_PCI_CLASS(klass);
  PCIDeviceClass *pcidev_k = PCI_DEVICE_CLASS(klass);
  k->realize = virtio_echo_pci_realize;
  set_bit(DEVICE_CATEGORY_MISC, dc->categories);
  pcidev_k->revision = VIRTIO_PCI_ABI_VERSION;
  pcidev_k->class_id = PCI_CLASS_OTHERS;
  dc->hotpluggable = false;
}

static const TypeInfo virtio_echo_pci_info = {
    .name = TYPE_VIRTIO_ECHO_PCI,
    .parent = TYPE_VIRTIO_PCI,
    .instance_size = sizeof(VirtIOechoPCI),
    .class_init = virtio_echo_pci_class_init,
    .instance_init = virtio_echo_pci_instance_init,
    .interfaces = (InterfaceInfo[]){{INTERFACE_PCIE_DEVICE}, {}}};

static void virtio_echo_type_init(void) {
  type_register_static(&virtio_echo_info);
  type_register_static(&virtio_echo_pci_info);
}

type_init(virtio_echo_type_init)
