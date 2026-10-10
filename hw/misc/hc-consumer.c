#include "qemu/osdep.h"

#include "glib.h"

#include "qemu/error-report.h"
#include "hw/core/qdev.h"
#include "hw/pci/msix.h"
#include "hw/pci/pci_device.h"
#include "hw/core/qdev-properties.h"
#include "qemu/host-utils.h"
#include "qemu/processor.h"
#include "qemu/thread.h"
#include "qemu/typedefs.h"
#include "qom/object.h"
#include "standard-headers/linux/pci_regs.h"
#include "system/memory.h"

#include <sched.h>
#include <string.h>
#include <sys/poll.h>

#define PC_POP_LOG(fmt, ...)                                                   \
  qemu_printf("[hc-consumer] POP " fmt "\n", __VA_ARGS__)

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
  uint64_t queue_len;

  Costs costs;

  char *output_path;
} HCConsumerConfig;

typedef struct {
  PCIDevice parent;

  HCConsumerConfig config;

  EventBuffer events;

  atomic_uint32_t cons_status;
  atomic_uint32_t prod_status;

  MemoryRegion regs_mr;
  EventNotifier doorbell;
  EventNotifier interrupt;

  MemoryRegion msix_mr;

  uint64_t shared_size;
  MemoryRegion shared_mr;
  SPSCQueue *queue;

  QemuThread worker_thread;
} HCConsumer;

static void hc_consumer_fix_hart(RuntimeMut self) {
  HCConsumer *cons = self.data;
  int cpu = sched_getcpu();

  if (cpu < 0) {
    error_report("sched_getcpu() failed: %s", strerror(errno));
    abort();
  }

  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);

  int ret = qemu_thread_set_affinity(
      &cons->worker_thread, (unsigned long *)&set, sizeof(set) * CHAR_BIT);

  if (ret < 0) {
    error_report("failed to pin worker thread to CPU %d: %s", cpu,
                 strerror(-ret));
    abort();
  }
}

static bool hc_consumer_is_interrupted(RuntimeConst self) {
  const HCConsumer *cons = self.data;
  return au32_load_acquire(&cons->cons_status) == HC_SIDE_STATUS_CLOSED;
}

static const RuntimeOps hc_consumer_runtime_ops = {
    .fix_hart = hc_consumer_fix_hart,
    .is_interrupted = hc_consumer_is_interrupted,
};

declare_impl(HCConsumer, Runtime, hc_consumer_runtime_ops);

static SPSCQueue *hc_consumer_get_queue(TransportConst self) {
  return ((HCConsumer *)self.data)->queue;
}

static int hc_consumer_ready_barrier_wait(TransportMut self) {
  HCConsumer *cons = self.data;
  au32_store_release(&cons->cons_status, HC_SIDE_STATUS_READY);
  for (;;) {
    if (au32_load_acquire(&cons->prod_status) == HC_SIDE_STATUS_READY) {
      return 0;
    }
    if (au32_load_acquire(&cons->cons_status) == HC_SIDE_STATUS_CLOSED) {
      return CRCLOSED;
    }
    cpu_relax();
  }
}

static int hc_consumer_wait_until(TransportMut self) {
  HCConsumer *cons = self.data;

  struct pollfd pfds[2] = {
      {
          .fd = event_notifier_get_fd(&cons->doorbell),
          .events = POLLIN,
      },
      {
          .fd = event_notifier_get_fd(&cons->interrupt),
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
      event_notifier_test_and_clear(&cons->doorbell);
      return 0;
    }

    if (pfds[1].revents & POLLIN) {
      event_notifier_test_and_clear(&cons->interrupt);
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

static int hc_consumer_notify(TransportMut self) {
  HCConsumer *cons = self.data;
  PCIDevice *pdev = PCI_DEVICE(cons);
  msix_notify(pdev, 0);
  return 0;
}

static bool hc_consumer_is_closed(TransportConst self) {
  const HCConsumer *qt = self.data;
  return au32_load_acquire(&qt->prod_status) == HC_SIDE_STATUS_CLOSED;
}

static void hc_consumer_close(TransportMut self) {
  HCConsumer *cons = self.data;
  au32_store_release(&cons->cons_status, HC_SIDE_STATUS_CLOSED);
  event_notifier_set(&cons->interrupt);
}

static const TransportOps hc_consumer_transport_ops = {
    .get_queue = hc_consumer_get_queue,
    .ready_barrier_wait = hc_consumer_ready_barrier_wait,
    .wait_until = hc_consumer_wait_until,
    .notify = hc_consumer_notify,
    .is_closed = hc_consumer_is_closed,
    .close = hc_consumer_close,
};

declare_impl(HCConsumer, Transport, hc_consumer_transport_ops);

static uint64_t hc_consumer_regs_read(void *opaque, hwaddr addr,
                                      unsigned size) {
  HCConsumer *cons = opaque;
  switch (addr) {
  case HC_REG_ABI_VERSION:
    return HC_ABI_VERSION_1_0;
  case HC_REG_MODE:
    return HC_MODE_NOTIFY;
  case HC_REG_CONS_STATUS:
    return au32_load_acquire(&cons->cons_status);
  case HC_REG_SHARED_SIZE:
    return cons->shared_size;
  default:
    /*
     * The rest is write-only.
     */
    return 0;
  }
}
static void hc_consumer_regs_write(void *opaque, hwaddr addr, uint64_t value,
                                   unsigned size) {
  HCConsumer *cons = opaque;
  switch (addr) {
  case HC_REG_PROD_STATUS:
    au32_store_release(&cons->prod_status, (uint32_t)value);
    break;
  case HC_REG_DOORBELL:
    /*
     * Handled by ioeventfd.
     */
    break;
  default:
    /*
     * The rest is read-only.
     */
    break;
  }
}

static const MemoryRegionOps regs_mr_ops = {
    .read = hc_consumer_regs_read,
    .write = hc_consumer_regs_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {.min_access_size = 4, .max_access_size = 8},
};

static void *consume(void *opaque) {
  int res;
  HCConsumer *cons = opaque;

  const ConsumerInfo info = {
      .costs = cons->config.costs,
      .runtime = HCConsumerAsRuntimeMut(cons),
      .transport = HCConsumerAsTransportMut(cons),
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

  int res = 0;
  uint64_t queue_len = 0;
  PerPacketEvents *event_data = NULL;
  HCConsumer *cons = HC_CONSUMER(pdev);

  queue_len = cons->config.queue_len;

  /*
   * Init struct
   */

  au32_store_release(&cons->cons_status, HC_SIDE_STATUS_PENDING);
  au32_store_release(&cons->prod_status, HC_SIDE_STATUS_PENDING);

  event_data = g_new0(PerPacketEvents, cons->config.event_len);
  if (event_data == NULL) {
    error_setg(errp, "failed to allocate the event buffer");
    return;
  }
  event_buffer_init(event_data, cons->config.queue_len, &cons->events);

  /*
   * Init event notifiers
   */

  res = event_notifier_init(&cons->doorbell, 0);
  if (res < 0) {
    error_setg(errp, "failed to create event notifiers: %s", strerror(res));
    goto err_free_events;
  }

  res = event_notifier_init(&cons->interrupt, 0);
  if (res < 0) {
    error_setg(errp, "failed to create event notifiers: %s", strerror(res));
    event_notifier_cleanup(&cons->doorbell);
    goto err_free_events;
  }

  /*
   * Initialize memory regions
   */

  cons->shared_size = pow2ceil(calculate_spsc_queue_size(queue_len));
  memory_region_init_io(&cons->regs_mr, OBJECT(cons), &regs_mr_ops, cons,
                        "hc-consumer-mem-regs", HC_BAR_SIZE_REGS);
  memory_region_init(&cons->msix_mr, OBJECT(cons), "hc-consumer-mem-msix",
                     HC_BAR_SIZE_MSIX);
  memory_region_init_ram(&cons->shared_mr, OBJECT(cons),
                         "qemu-transport-mem-shared", cons->shared_size, errp);
  if (*errp) {
    goto err_free_events;
  }

  memory_region_add_eventfd(&cons->regs_mr, HC_REG_DOORBELL, 4, false, 0,
                            &cons->doorbell);

  /*
   * Initialize SPSC queue
   */

  cons->queue = memory_region_get_ram_ptr(&cons->shared_mr);
  spsc_queue_init(cons->queue, queue_len);

  /*
   * Register BARs
   */
  pci_register_bar(pdev, HC_BAR_REGS, PCI_BASE_ADDRESS_SPACE_MEMORY,
                   &cons->regs_mr);
  res = msix_init(pdev, 1, &cons->msix_mr, HC_BAR_MSIX, HC_MSIX_TABLE_OFFSET,
                  &cons->msix_mr, HC_BAR_MSIX, HC_MSIX_PBA_OFFSET, 0, errp);
  if (res < 0) {
    goto err_del_eventfds;
  }
  msix_vector_use(pdev, 0);
  pci_register_bar(pdev, HC_BAR_MSIX, PCI_BASE_ADDRESS_SPACE_MEMORY,
                   &cons->msix_mr);
  pci_register_bar(pdev, HC_BAR_SHARED,
                   PCI_BASE_ADDRESS_SPACE_MEMORY |
                       PCI_BASE_ADDRESS_MEM_PREFETCH,
                   &cons->shared_mr);

  /*
   * Start worker
   */
  qemu_thread_create(&cons->worker_thread, "qemu-runtime-worker", consume, cons,
                     QEMU_THREAD_JOINABLE);

  return;

err_del_eventfds:
  memory_region_del_eventfd(&cons->regs_mr, HC_REG_DOORBELL, 4, false, 0,
                            &cons->doorbell);
  event_notifier_cleanup(&cons->doorbell);
  event_notifier_cleanup(&cons->interrupt);
err_free_events:
  g_free(cons->events.data);
}

static void hc_consumer_unrealize(PCIDevice *pdev) {
  HCConsumer *cons = HC_CONSUMER(pdev);

  hc_consumer_close(HCConsumerAsTransportMut(cons));
  qemu_thread_join(&cons->worker_thread);

  memory_region_del_eventfd(&cons->regs_mr, HC_REG_DOORBELL, 4, false, 0,
                            &cons->doorbell);
  event_notifier_cleanup(&cons->doorbell);
  event_notifier_cleanup(&cons->interrupt);

  msix_vector_unuse(pdev, 0);
  msix_uninit(pdev, &cons->msix_mr, &cons->msix_mr);

  int fd =
      qemu_open(cons->config.output_path, O_WRONLY | O_CREAT | O_TRUNC, NULL);
  if (fd < 0) {
    error_report("failed to create output file: %s", strerror(errno));
    goto skip_output;
  }

  for (uint64_t i = 0; i < cons->events.len; i++) {
    PerPacketEvents event = cons->events.data[i];
    dprintf(fd,
            "%lu,%lu,%lu,%lu,%lu,%lu"
            "%lu,%lu,%lu,%lu,%lu,%lu\n",
            event.producer.yield_start, event.producer.yield_end,
            event.producer.work_start, event.producer.work_end,
            event.producer.notify_start, event.producer.notify_end,
            event.consumer.yield_start, event.consumer.yield_end,
            event.consumer.work_start, event.consumer.work_end,
            event.consumer.notify_start, event.consumer.notify_end);
  }
  close(fd);
skip_output:
  g_free(cons->events.data);
}

static void hc_consumer_init(Object *obj) {
  HCConsumer *cons = HC_CONSUMER(obj);
  cons->config.output_path = g_strdup("./output.csv");
}

static const Property hc_consumer_properties[] = {
    DEFINE_PROP_UINT64("event-len", HCConsumer, config.event_len, 1024),
    DEFINE_PROP_UINT64("queue-len", HCConsumer, config.queue_len, 1024),
    DEFINE_PROP_UINT64("work-cost", HCConsumer, config.costs.work, 3000),

    DEFINE_PROP_UINT64("yield-cost", HCConsumer, config.costs.yield, 3000),
    DEFINE_PROP_UINT64("resume-cost", HCConsumer, config.costs.resume, 3000),
    DEFINE_PROP_UINT64("notify-cost", HCConsumer, config.costs.notify, 3000),

    DEFINE_PROP_STRING("output-path", HCConsumer, config.output_path),
};

static void hc_consumer_class_init(ObjectClass *klass, const void *data) {
  DeviceClass *dc = DEVICE_CLASS(klass);
  PCIDeviceClass *pdc = PCI_DEVICE_CLASS(klass);

  dc->desc = "Consumer-side implementation of a hypercall-based SPSC loop for"
             "QEMU.";
  device_class_set_props(dc, hc_consumer_properties);

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
    .instance_init = hc_consumer_init,
    .interfaces =
        (const InterfaceInfo[]){{INTERFACE_CONVENTIONAL_PCI_DEVICE}, {}},
};

static void hc_consumer_type_init(void) {
  type_register_static(&hc_consumer_info);
}

type_init(hc_consumer_type_init)
