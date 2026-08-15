#include "qemu/osdep.h"
#include "hw/core/qdev.h"
#include "hw/virtio/vhost.h"
#include "hw/virtio/virtio.h"
#include "hw/virtio/virtio-pci.h"
#include "qemu/typedefs.h"
#include "standard-headers/linux/virtio_config.h"
#include "standard-headers/linux/virtio_ids.h"
#include <stddef.h>
#include <stdint.h>

#define TYPE_VIRTIO_PC_CONSUMER "virtio-pc-consumer"
#define TYPE_VIRTIO_PC_CONSUMER_PCI "virtio-pc-consumer-pci"
#define VIRTIO_PC_CONSUMER(obj)                                                \
  OBJECT_CHECK(VirtIOPCConsumer, obj, TYPE_VIRTIO_PC_CONSUMER)
#define VIRTIO_PC_CONSUMER_PCI(obj)                                            \
  OBJECT_CHECK(VirtIOPCConsumerPCI, obj, TYPE_VIRTIO_PC_CONSUMER_PCI)

struct virtio_pc_config {
  uint32_t work_queue_len;
  uint64_t work_cost_consumption;

} virtio_pc_config;

typedef struct VirtIOPCConsumer {
  VirtIODevice parent_obj;

  VirtQueue *wq;
  QEMUBH *bh;

  struct virtio_pc_config config;
} VirtIOPCConsumer;

typedef struct VirtIOPCConsumerPCI {
  VirtIOPCIProxy parent_obj;
  VirtIOPCConsumer vdev;
} VirtIOPCConsumerPCI;

static const VMStateDescription vmstate_virtio_pc_consumer = {
    .name = "virtio-pc",
    .minimum_version_id = 1,
    .version_id = 1,
    .fields = (VMStateField[]){VMSTATE_VIRTIO_DEVICE, VMSTATE_END_OF_LIST()},
};

static Property virtio_pc_consumer_properties[] = {
    DEFINE_PROP_UINT32("wq_len", VirtIOPCConsumer, config.work_queue_len, 256),
    DEFINE_PROP_UINT64("wc_c", VirtIOPCConsumer, config.work_cost_consumption,
                       3000),
};

static inline uint64_t rdtsc(void) {
  uint32_t hi, lo;
  __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
  return (uint64_t)lo | ((uint64_t)hi << 32);
}

static uint64_t do_work(uint64_t cost) {
  uint64_t until = rdtsc() + cost;
  while (rdtsc() < until)
    barrier();
}

static void virtio_pc_consumer_main_thread(void *opaque) {
  VirtIOPCConsumer *cons = opaque;
  VirtIODevice *vdev = VIRTIO_DEVICE(cons);
  VirtQueue *wq = cons->wq;
  uint64_t work_cost = cons->config.work_cost_consumption;

  if (!vdev->vm_running) {
    return;
  }

  if (unlikely(!(vdev->status & VIRTIO_CONFIG_S_DRIVER_OK))) {
    return;
  }

  for (;;) {
    VirtQueueElement *elem;

    elem = virtqueue_pop(wq, sizeof(VirtQueueElement));
    if (!elem) {
      break;
    }
    virtqueue_push(wq, elem, 0);
    g_free(elem);

    do_work(work_cost);
  }

  virtio_queue_set_notification(wq, 1);
}

static void virtio_pc_consumer_on_wq_notify(VirtIODevice *vdev, VirtQueue *vq) {
  VirtIOPCConsumer *cons = VIRTIO_PC_CONSUMER(vdev);

  if (!vdev->vm_running) {
    return;
  }

  virtio_queue_set_notification(cons->wq, 0);
  qemu_bh_schedule(cons->bh);
}

static void virtio_pc_consumer_realize(DeviceState *dev, Error **errp) {
  VirtIODevice *vdev = VIRTIO_DEVICE(dev);
  VirtIOPCConsumer *cons = VIRTIO_PC_CONSUMER(dev);

  virtio_init(vdev, VIRTIO_ID_ECHO, sizeof(struct virtio_pc_config));

  cons->wq = virtio_add_queue(vdev, cons->config.work_queue_len,
                              virtio_pc_consumer_on_wq_notify);
  cons->bh = qemu_bh_new_guarded(virtio_pc_consumer_main_thread, cons,
                                 &dev->mem_reentrancy_guard);
}

static void virtio_pc_consumer_unrealize(DeviceState *dev) {
  VirtIODevice *vdev = VIRTIO_DEVICE(dev);
  VirtIOPCConsumer *cons = VIRTIO_PC_CONSUMER(cons);

  qemu_bh_delete(cons->bh);
  virtio_del_queue(vdev, 0);
  virtio_cleanup(vdev);
}

static void virtio_pc_consumer_set_config(VirtIODevice *vdev,
                                          const uint8_t *config) {}

static uint64_t virtio_pc_consumer_get_features(VirtIODevice *vdev,
                                                uint64_t requested_features,
                                                Error **errp) {
  return requested_features;
}

static void virtio_pc_consumer_set_features(VirtIODevice *vdev,
                                            uint64_t features) {}

static uint64_t virtio_pc_consumer_bad_features(VirtIODevice *vdev) {
  return 0;
}

static void virtio_pc_consumer_instance_init(Object *obj) {}

static void virtio_pc_consumer_class_init(ObjectClass *klass,
                                          const void *data) {
  DeviceClass *dc = DEVICE_CLASS(klass);
  VirtioDeviceClass *vdc = VIRTIO_DEVICE_CLASS(klass);

  dc->vmsd = &vmstate_virtio_pc_consumer;
  device_class_set_props(dc, virtio_pc_consumer_properties);
  set_bit(DEVICE_CATEGORY_MISC, dc->categories);
  vdc->realize = virtio_pc_consumer_realize;
  vdc->unrealize = virtio_pc_consumer_unrealize;
  vdc->set_config = virtio_pc_consumer_set_config;
  vdc->get_features = virtio_pc_consumer_get_features;
  vdc->set_features = virtio_pc_consumer_set_features;
  vdc->bad_features = virtio_pc_consumer_bad_features;
}

static const TypeInfo virtio_pc_consumer_info = {
    .name = TYPE_VIRTIO_PC_CONSUMER,
    .parent = TYPE_VIRTIO_DEVICE,
    .instance_size = sizeof(VirtIOPCConsumer),
    .instance_init = virtio_pc_consumer_instance_init,
    .class_init = virtio_pc_consumer_class_init,
};

static void virtio_pc_consumer_pci_realize(VirtIOPCIProxy *vpci_dev,
                                           Error **errp) {
  VirtIOPCConsumerPCI *dev =
      container_of(vpci_dev, VirtIOPCConsumerPCI, parent_obj);
  DeviceState *vdev = DEVICE(&dev->vdev);

  virtio_pci_force_virtio_1(vpci_dev);
  qdev_realize(vdev, BUS(&vpci_dev->bus), errp);
}

static void virtio_pc_consumer_pci_instance_init(Object *obj) {
  VirtIOPCConsumerPCI *dev = VIRTIO_PC_CONSUMER_PCI(obj);

  virtio_instance_init_common(obj, &dev->vdev, sizeof(dev->vdev),
                              TYPE_VIRTIO_PC_CONSUMER);
}

static void virtio_pc_consumer_pci_class_init(ObjectClass *klass,
                                              const void *data) {
  DeviceClass *dc = DEVICE_CLASS(klass);
  VirtioPCIClass *k = VIRTIO_PCI_CLASS(klass);
  PCIDeviceClass *pcidev_k = PCI_DEVICE_CLASS(klass);
  k->realize = virtio_pc_consumer_pci_realize;
  set_bit(DEVICE_CATEGORY_MISC, dc->categories);
  pcidev_k->revision = VIRTIO_PCI_ABI_VERSION;
  pcidev_k->class_id = PCI_CLASS_OTHERS;
  dc->hotpluggable = false;
}

static const TypeInfo virtio_pc_pci_consumer_info = {
    .name = TYPE_VIRTIO_PC_CONSUMER_PCI,
    .parent = TYPE_VIRTIO_PCI,
    .instance_size = sizeof(VirtIOPCConsumerPCI),
    .class_init = virtio_pc_consumer_pci_class_init,
    .instance_init = virtio_pc_consumer_pci_instance_init,
    .interfaces = (InterfaceInfo[]){{INTERFACE_PCIE_DEVICE}, {}}};

static void virtio_pc_type_init(void) {
  type_register_static(&virtio_pc_consumer_info);
  type_register_static(&virtio_pc_pci_consumer_info);
}

type_init(virtio_pc_type_init)
