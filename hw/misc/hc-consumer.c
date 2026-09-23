#include "qemu/osdep.h"

#include "glib.h"

#include "qemu/error-report.h"
#include "hw/core/qdev.h"
#include "hw/pci/msix.h"
#include "hw/pci/pci_device.h"
#include "qemu/thread.h"
#include "qemu/typedefs.h"
#include "qom/object.h"
#include "standard-headers/linux/pci_regs.h"
#include "system/hostmem.h"
#include "system/memory.h"

#include <sched.h>
#include <sys/poll.h>

#include "contrib/producer-consumer/core/portable.h"
#include "contrib/producer-consumer/core/spsc_queue.h"
#include "contrib/producer-consumer/core/transport.h"
#include "contrib/producer-consumer/core/measurement.h"
#include "contrib/producer-consumer/core/common.h"
#include "contrib/producer-consumer/core/consumer.h"
#include "contrib/producer-consumer/hc-common/abi.h"

#define TYPE_HC_CONSUMER "hc-consumer"
#define HC_CONSUMER(obj) OBJECT_CHECK(HCConsumer, obj, TYPE_HC_CONSUMER)

typedef struct {
  uint64_t event_len;

  HostMemoryBackend *queue_memdev;
  uint64_t queue_len;

  uint64_t work_cost;
} HCConsumerConfig;

typedef struct {
  QemuThread worker_thread;
  atomic_bool_t interrupted;
  EventNotifier *wakeup;
} QemuRuntime;

typedef struct {
  PCIDevice *owner;

  MemoryRegion regs_mr;
  EventNotifier doorbell;
  EventNotifier wakeup;

  MemoryRegion msix_mr;

  uint64_t shared_size;
  MemoryRegion shared_mr;
  SPSCQueue *queue;
} HCTransport;

typedef struct {
  PCIDevice parent;

  HCConsumerConfig config;

  QemuRuntime runtime;
  EventBuffer events;
  HCTransport transport;
} HCConsumer;

static void qemu_runtime_fix_hart(RuntimeMut self) {}

static bool qemu_runtime_is_interrupted(RuntimeConst self) {
  const QemuRuntime *qr = self.data;
  return ab_load_acquire(&qr->interrupted);
}

static const RuntimeOps qemu_runtime_ops = {
    .fix_hart = qemu_runtime_fix_hart,
    .is_interrupted = qemu_runtime_is_interrupted,
};

declare_impl(QemuRuntime, Runtime, qemu_runtime_ops);

static void qemu_runtime_interrupt(QemuRuntime *self) {
  ab_store_release(&self->interrupted, true);
  event_notifier_set(self->wakeup);
}

static void qemu_runtime_init(QemuRuntime *self, void *(*main_func)(void *),
                              void *arg, EventNotifier *wakeup) {
  ab_store_release(&self->interrupted, false);
  self->wakeup = wakeup;

  qemu_thread_create(&self->worker_thread, "qemu-runtime-worker", main_func,
                     arg, QEMU_THREAD_JOINABLE);
}

static void qemu_runtime_deinit(QemuRuntime *self) {
  qemu_runtime_interrupt(self);
  qemu_thread_join(&self->worker_thread);

  // For debugging
  *self = (QemuRuntime){0};
}

static SPSCQueue *hc_transport_get_queue(TransportConst self) {
  return ((HCTransport *)self.data)->queue;
}

static int hc_transport_wait_until(TransportMut self) {
  HCTransport *qt = self.data;

  struct pollfd pfds[2] = {
      {
          .fd = event_notifier_get_fd(&qt->doorbell),
          .events = POLLIN,
      },
      {
          .fd = event_notifier_get_fd(&qt->wakeup),
          .events = POLLIN,
      },
  };

  for (;;) {
    int ret = poll(pfds, ARRAY_SIZE(pfds), -1);

    if (ret < 0 && errno == EINTR) {
      continue;
    }

    if (ret < 0) {
      return -errno;
    }

    if (pfds[0].revents & POLLIN) {
      event_notifier_test_and_clear(&qt->doorbell);
      return 0;
    }

    if (pfds[1].revents & POLLIN) {
      event_notifier_test_and_clear(&qt->wakeup);
      return CRINTERRUPTED;
    }

    if (pfds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
      return -EIO;
    }

    if (pfds[1].revents & (POLLERR | POLLHUP | POLLNVAL)) {
      return -EIO;
    }
  }
}

static int hc_transport_notify(TransportMut self) {

  HCTransport *qt = self.data;
  msix_notify(qt->owner, 0);
  return 0;
}

static bool hc_transport_is_closed(TransportConst self) {
  /* TBD */
  return false;
}

static const TransportOps hc_transport_ops = {
    .get_queue = hc_transport_get_queue,
    .wait_until = hc_transport_wait_until,
    .notify = hc_transport_notify,
    .is_closed = hc_transport_is_closed,
};

declare_impl(HCTransport, Transport, hc_transport_ops);

static uint64_t hc_transport_regs_read(void *opaque, hwaddr addr,
                                       unsigned size) {
  HCTransport *qt = opaque;
  switch (addr) {
  case QT_REG_VERSION:
    return 0;
  case QT_REG_STATUS:
    return 0;
  case QT_REG_SHARED_SIZE:
    return qt->shared_size;
  default:
    return 0;
  }
}
static void hc_transport_regs_write(void *opaque, hwaddr addr, uint64_t value,
                                    unsigned size) {
  /*
   * DOORBELL is handled by ioeventfd, and the other registers are read-only
   */
}

static const MemoryRegionOps regs_mr_ops = {
    .read = hc_transport_regs_read,
    .write = hc_transport_regs_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {.min_access_size = 4, .max_access_size = 8},
};

static bool hc_transport_init(HCTransport *self, PCIDevice *owner,
                              uint64_t queue_len, MemoryRegion *backend_mr,
                              Error **errp) {
  int res = 0;
  uint64_t shared_size = 0, avail_len = 0;
  ERRP_GUARD();

  shared_size = memory_region_size(backend_mr);
  if (!is_power_of_2(shared_size)) {
    error_setg(errp, "shared memory size must be a power of two");
    goto err;
  }

  avail_len = calculate_spsc_queue_capacity_for_size(shared_size);
  if (avail_len < queue_len) {
    error_setg(errp, "shared memory has insufficient size");
    goto err;
  }

  self->owner = owner;

  res = event_notifier_init(&self->wakeup, 0);
  if (res < 0) {
    error_setg(errp, "failed to create wakeup event notifier: %s",
               strerror(res));
    goto err;
  }

  /*
   * BAR0: Registers (0x1000)
   */

  memory_region_init_io(&self->regs_mr, OBJECT(owner), &regs_mr_ops, self,
                        "qemu-transport-regs-mem", 0x1000);

  res = event_notifier_init(&self->doorbell, 0);
  if (res < 0) {
    error_setg(errp, "failed to create doorbell event notifier: %s",
               strerror(res));
    goto err_cleanup_wakeup;
  }
  memory_region_add_eventfd(&self->regs_mr, QT_REG_DOORBELL, 4, false, 0,
                            &self->doorbell);

  pci_register_bar(owner, QT_BAR_REGS, PCI_BASE_ADDRESS_SPACE_MEMORY,
                   &self->regs_mr);

  /*
   * BAR1: MSI-X (0x1000)
   */

  memory_region_init(&self->msix_mr, OBJECT(owner), "qemu-transport-msix-mem",
                     QT_MSIX_BAR_SIZE);

  res = msix_init(owner, 1, &self->msix_mr, QT_BAR_MSIX, QT_MSIX_TABLE_OFFSET,
                  &self->msix_mr, QT_BAR_MSIX, QT_MSIX_PBA_OFFSET, 0, errp);
  if (res < 0) {
    goto err_cleanup_doorbell;
  }
  msix_vector_use(owner, 0);

  /*
   * BAR2: Shared memory
   */

  self->shared_size = shared_size;
  memory_region_init_alias(&self->shared_mr, OBJECT(owner),
                           "qemu-transport-shared-mem", backend_mr, 0,
                           shared_size);

  self->queue = memory_region_get_ram_ptr(backend_mr);
  spsc_queue_init(self->queue, queue_len);

  pci_register_bar(owner, QT_BAR_SHARED,
                   PCI_BASE_ADDRESS_SPACE_MEMORY |
                       PCI_BASE_ADDRESS_MEM_PREFETCH,
                   &self->shared_mr);

  return true;

err_cleanup_doorbell:
  memory_region_del_eventfd(&self->regs_mr, QT_REG_DOORBELL, 4, false, 0,
                            &self->doorbell);
  event_notifier_cleanup(&self->doorbell);
err_cleanup_wakeup:
  event_notifier_cleanup(&self->wakeup);
err:
  return false;
}

static void hc_transport_deinit(HCTransport *self) {
  memory_region_del_eventfd(&self->regs_mr, QT_REG_DOORBELL, 4, false, 0,
                            &self->doorbell);
  event_notifier_cleanup(&self->doorbell);

  msix_vector_unuse(self->owner, 0);
  msix_uninit(self->owner, &self->msix_mr, &self->msix_mr);

  event_notifier_cleanup(&self->wakeup);

  // For debugging
  *self = (HCTransport){0};
}

static void *consume(void *opaque) {
  int res;
  HCConsumer *cons = opaque;

  const ConsumerInfo info = {
      .work_cost = cons->config.work_cost,
      .runtime = QemuRuntimeAsRuntimeMut(&cons->runtime),
      .transport = HCTransportAsTransportMut(&cons->transport),
      .events = &cons->events,
  };

  res = run_consumer(&info);
  switch (res) {
  case CREND:
    info_report("hc-consumer: event buffer was successfuly filled");
    goto success;
  case CRCLOSED:
    info_report("hc-consumer: transport was closed from the other side");
    goto success;
  case CRINTERRUPTED:
    info_report("hc-consumer: received interrupt signal");
    goto success;
  default:
    error_report("hc-consumer: unexpected error: %s", strerror(-res));
    goto failure;
  }

success:
  return NULL;
failure:
  return NULL;
}

static void hc_consumer_realize(PCIDevice *pdev, Error **errp) {
  ERRP_GUARD();
  HCConsumer *cons = HC_CONSUMER(pdev);

  if (!cons->config.queue_memdev) {
    error_setg(errp, "queue_memdev is required");
    return;
  }

  MemoryRegion *mr = host_memory_backend_get_memory(cons->config.queue_memdev);
  if (!mr) {
    error_setg(errp, "invalid memory backend");
    return;
  }

  if (!hc_transport_init(&cons->transport, pdev, cons->config.queue_len, mr,
                         errp)) {
    return;
  }

  event_buffer_init(g_new0(PerPacketEvents, cons->config.queue_len),
                    cons->config.queue_len, &cons->events);

  qemu_runtime_init(&cons->runtime, consume, cons, &cons->transport.wakeup);
}

static void hc_consumer_unrealize(PCIDevice *pdev) {
  HCConsumer *cons = HC_CONSUMER(pdev);

  qemu_runtime_deinit(&cons->runtime);
  hc_transport_deinit(&cons->transport);

  g_free(cons->events.data);
}

static void hc_consumer_class_init(ObjectClass *klass, const void *data) {
  DeviceClass *dc = DEVICE_CLASS(klass);
  PCIDeviceClass *pdc = PCI_DEVICE_CLASS(klass);

  dc->desc = "Consumer-side implementation of a hypercall-based SPSC loop for"
             "QEMU.";
  pdc->device_id = HCC_DEVICE_ID;
  pdc->vendor_id = HCC_VENDOR_ID;
  pdc->revision = 0x01;
  pdc->class_id = PCI_CLASS_OTHERS;

  pdc->realize = hc_consumer_realize;
  pdc->exit = hc_consumer_unrealize;
}

static const TypeInfo hc_consumer_info = {
    .name = TYPE_HC_CONSUMER,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(HCConsumer),
    .class_init = hc_consumer_class_init,
};

static void hc_consumer_type_init(void) {
  type_register_static(&hc_consumer_info);
}

type_init(hc_consumer_type_init)
