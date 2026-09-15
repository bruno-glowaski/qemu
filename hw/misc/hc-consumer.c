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

#define TYPE_HC_CONSUMER "hc-consumer"
#define HC_CONSUMER(obj) OBJECT_CHECK(HCConsumer, obj, TYPE_HC_CONSUMER)

#define HCC_VENDOR_ID 0x1234
#define HCC_DEVICE_ID 0x5678

#define QT_BAR_REGS 0
#define QT_BAR_MSIX 1
#define QT_BAR_SHARED 2

// RO 32-bit
#define QT_REG_VERSION 0x000
// RO 32-bit
#define QT_REG_STATUS 0x008
// RO 64-bit
#define QT_REG_SHARED_SIZE 0x010
// WO 32-bit
#define QT_REG_DOORBELL 0x014

#define QT_MSIX_BAR_SIZE 0x1000
#define QT_MSIX_TABLE_OFFSET 0x0000
#define QT_MSIX_PBA_OFFSET 0x0800

typedef struct {
  uint64_t event_len;

  HostMemoryBackend *queue_memdev;
  uint64_t queue_len;

  uint64_t work_cost;
} HCConsumerConfig;

typedef struct {
  PCIDevice parent;

  HCConsumerConfig config;

  Runtime runtime;
  EventBuffer events;
  Transport transport;
} HCConsumer;

typedef struct {
  PCIDevice *owner;

  MemoryRegion regs_mr;
  EventNotifier doorbell;

  MemoryRegion msix_mr;

  uint64_t shared_size;
  MemoryRegion shared_mr;
  SPSCQueue *queue;
} QemuTransport;

static SPSCQueue *qemu_transport_get_queue(const Transport *self) {
  return ((QemuTransport *)self->data)->queue;
}

static int qemu_transport_wait_until(Transport *self) {
  QemuTransport *qt = self->data;

  struct pollfd pfd = {
      .fd = event_notifier_get_fd(&qt->doorbell),
      .events = POLLIN,
  };

  for (;;) {
    int ret = poll(&pfd, 1, -1);

    if (ret < 0 && errno == EINTR) {
      continue;
    }

    if (ret < 0) {
      return -errno;
    }

    if (pfd.revents & POLLIN) {
      event_notifier_test_and_clear(&qt->doorbell);
      return 0;
    }
  }
}

static int qemu_transport_notify(Transport *self) {

  QemuTransport *qt = self->data;
  msix_notify(qt->owner, 0);
  return 0;
}

static bool qemu_transport_is_closed(const Transport *self) {
  /* TBD */
  return false;
}

static const TransportOps qemu_transport_ops = {
    .get_queue = qemu_transport_get_queue,
    .wait_until = qemu_transport_wait_until,
    .notify = qemu_transport_notify,
    .is_closed = qemu_transport_is_closed,
};

static uint64_t qemu_transport_regs_read(void *opaque, hwaddr addr,
                                         unsigned size) {
  Transport *t = opaque;
  QemuTransport *qt = t->data;
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
static void qemu_transport_regs_write(void *opaque, hwaddr addr, uint64_t value,
                                      unsigned size) {
  /*
   * DOORBELL is handled by ioeventfd, and the other registers are read-only
   */
}

static const MemoryRegionOps regs_mr_ops = {
    .read = qemu_transport_regs_read,
    .write = qemu_transport_regs_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {.min_access_size = 4, .max_access_size = 8},
};

static bool qemu_transport_create(PCIDevice *owner, uint64_t queue_len,
                                  MemoryRegion *backend_mr, Transport *self,
                                  Error **errp) {
  int res = 0;
  uint64_t aval_queue_capacity = 0, shared_size = 0;
  ERRP_GUARD();

  self->data = NULL;
  self->ops = NULL;

  shared_size = memory_region_size(backend_mr);
  if (!is_power_of_2(shared_size)) {
    error_setg(errp, "shared memory size must be a power of two");
    goto err;
  }

  aval_queue_capacity = calculate_spsc_queue_capacity_for_size(shared_size);
  if (aval_queue_capacity < queue_len) {
    error_setg(errp, "shared memory has insufficient size");
    goto err;
  }

  QemuTransport *data;
  data = g_new0(QemuTransport, 1);
  data->owner = owner;

  /*
   * BAR0: Registers (0x1000)
   */

  memory_region_init_io(&data->regs_mr, OBJECT(owner), &regs_mr_ops, self,
                        "qemu-transport-regs-mem", 0x1000);

  res = event_notifier_init(&data->doorbell, 0);
  if (res < 0) {
    error_setg(errp, "failed to initialize ioeventfd: %s", strerror(res));
    goto err_free_data;
  }
  memory_region_add_eventfd(&data->regs_mr, QT_REG_DOORBELL, 4, false, 0,
                            &data->doorbell);

  pci_register_bar(owner, QT_BAR_REGS, PCI_BASE_ADDRESS_SPACE_MEMORY,
                   &data->regs_mr);

  /*
   * BAR1: MSI-X (0x1000)
   */

  memory_region_init(&data->msix_mr, OBJECT(owner), "qemu-transport-msix-mem",
                     QT_MSIX_BAR_SIZE);

  res = msix_init(owner, 1, &data->msix_mr, QT_BAR_MSIX, QT_MSIX_TABLE_OFFSET,
                  &data->msix_mr, QT_BAR_MSIX, QT_MSIX_PBA_OFFSET, 0, errp);
  if (res < 0) {
    goto err_destroy_doorbell;
  }
  msix_vector_use(owner, 0);

  /*
   * BAR2: Shared memory
   */

  data->shared_size = shared_size;
  memory_region_init_alias(&data->shared_mr, OBJECT(owner),
                           "qemu-transport-shared-mem", backend_mr, 0,
                           shared_size);

  data->queue = memory_region_get_ram_ptr(backend_mr);
  spsc_queue_init(data->queue, queue_len);

  pci_register_bar(owner, QT_BAR_SHARED,
                   PCI_BASE_ADDRESS_SPACE_MEMORY |
                       PCI_BASE_ADDRESS_MEM_PREFETCH,
                   &data->shared_mr);

  self->data = data;
  self->ops = &qemu_transport_ops;

  return true;

err_destroy_doorbell:
  memory_region_del_eventfd(&data->regs_mr, QT_REG_DOORBELL, 4, false, 0,
                            &data->doorbell);
  event_notifier_cleanup(&data->doorbell);
err_free_data:
  g_free(data);
err:
  return false;
}

static void qemu_transport_destroy(Transport *self) {
  QemuTransport *qt = self->data;

  memory_region_del_eventfd(&qt->regs_mr, QT_REG_DOORBELL, 4, false, 0,
                            &qt->doorbell);
  event_notifier_cleanup(&qt->doorbell);

  msix_vector_unuse(qt->owner, 0);
  msix_uninit(qt->owner, &qt->msix_mr, &qt->msix_mr);

  g_free(qt);

  self->data = NULL;
  self->ops = NULL;
}

typedef struct {
  QemuThread worker_thread;
  atomic_bool_t interrupted;
} QemuRuntime;

static void qemu_runtime_fix_hart(Runtime *self) {}

static bool qemu_runtime_is_interrupted(const Runtime *self) {
  QemuRuntime *qr = self->data;
  return ab_load_acquire(&qr->interrupted);
}

static const RuntimeOps qemu_runtime_ops = {
    .fix_hart = qemu_runtime_fix_hart,
    .is_interrupted = qemu_runtime_is_interrupted,
};

static void qemu_runtime_create(void *(*main_func)(void *), void *arg,
                                Runtime *self) {
  self->ops = NULL;
  self->data = NULL;

  QemuRuntime *data;
  data = g_new0(QemuRuntime, 1);

  ab_store_release(&data->interrupted, false);

  self->data = data;
  self->ops = &qemu_runtime_ops;

  qemu_thread_create(&data->worker_thread, "qemu-runtime-worker", main_func,
                     arg, QEMU_THREAD_JOINABLE);
}

static void qemu_runtime_interrupt(Runtime *self) {
  QemuRuntime *qr = self->data;
  ab_store_release(&qr->interrupted, true);
}

static void qemu_runtime_destroy(Runtime *self) {
  QemuRuntime *qr = self->data;

  ab_store_release(&qr->interrupted, true);
  qemu_thread_join(&qr->worker_thread);

  g_free(qr);

  self->data = NULL;
  self->ops = NULL;
}

static void *consume(void *opaque) {
  int res;
  HCConsumer *cons = opaque;

  const ConsumerInfo info = {
      .work_cost = cons->config.work_cost,
      .runtime = &cons->runtime,
      .transport = &cons->transport,
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

  if (!qemu_transport_create(pdev, cons->config.queue_len, mr, &cons->transport,
                             errp)) {
    return;
  }

  event_buffer_init(g_new0(PerPacketEvents, cons->config.queue_len),
                    cons->config.queue_len, &cons->events);

  qemu_runtime_create(consume, cons, &cons->runtime);
}

static void hc_consumer_unrealize(PCIDevice *pdev) {
  HCConsumer *cons = HC_CONSUMER(pdev);

  qemu_runtime_destroy(&cons->runtime);
  qemu_transport_destroy(&cons->transport);

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
