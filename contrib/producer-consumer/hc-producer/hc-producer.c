#include <linux/module.h>

#include <linux/miscdevice.h>
#include <linux/pci.h>

#include "../core/portable.h"
#include "../core/transport.h"
#include "../core/producer.h"
#include "../hc-common/abi.h"
#include "../linux-kernel/common.h"
#include "asm-generic/errno-base.h"

#define HCP_IOCTL_RUN 0

static const struct pci_device_id hc_ids[] = {
    {PCI_DEVICE(HCC_VENDOR_ID, HCC_DEVICE_ID)}, {}};

MODULE_DEVICE_TABLE(pci, hc_ids);

typedef struct LinuxHCTransport {
  struct pci_dev *owner;

  wait_queue_head_t waitq;

  void __iomem *regs;
  void __iomem *msix;
  // Doesn't need __iomem because this memory is guaranteed to be cacheable.
  void *shared;
  uint64_t shared_size;

  SPSCQueue *queue;
  int irq;
} LinuxHCTransport;

struct hc_producer_device {
  struct pci_dev *pdev;

  struct miscdevice miscdev;

  LinuxHCTransport transport;

  struct mutex run_lock;
};

static irqreturn_t hc_irq(int irq, void *data) {
  LinuxHCTransport *lt = data;

  wake_up_interruptible(&lt->waitq);

  return IRQ_HANDLED;
}

static SPSCQueue *linux_hc_transport_get_queue(TransportConst self) {
  const LinuxHCTransport *lt = self.data;

  return lt->queue;
}

static int linux_hc_transport_notify(TransportMut self) {
  LinuxHCTransport *lt = self.data;

  writel(0, lt->regs + QT_REG_DOORBELL);

  return 0;
}

static bool linux_hc_transport_is_closed(TransportConst self) { return false; }

static int linux_hc_transport_wait_until(TransportMut self) {
  int ret;
  LinuxHCTransport *lt = self.data;

  spsc_queue_request_signal_for_producer(lt->queue);
  ret = wait_event_interruptible(
      lt->waitq, spsc_queue_peek_push(lt->queue) != NULL ||
                     linux_hc_transport_is_closed(AsTransportConst(self)));
  spsc_queue_clear_signal_for_producer(lt->queue);

  if (ret == -EINTR) {
    return CRINTERRUPTED;
  }

  return ret;
}

static const TransportOps linux_hc_transport_ops = {
    .get_queue = linux_hc_transport_get_queue,
    .is_closed = linux_hc_transport_is_closed,
    .wait_until = linux_hc_transport_wait_until,
    .notify = linux_hc_transport_notify,
};

declare_impl(LinuxHCTransport, Transport, linux_hc_transport_ops);

static int linux_hc_transport_init(LinuxHCTransport *self,
                                   struct pci_dev *owner) {
  int ret;

  if (WARN_ON_ONCE(
          !(pci_resource_flags(owner, QT_BAR_SHARED) & IORESOURCE_PREFETCH))) {
    dev_err(&owner->dev, "BAR2 is not prefetchable\n");
    return -ENODEV;
  }

  self->owner = owner;
  init_waitqueue_head(&self->waitq);

  ret = pcim_enable_device(owner);
  if (ret)
    return ret;

  ret = pcim_iomap_regions(
      owner, BIT(QT_BAR_REGS) | BIT(QT_BAR_MSIX) | BIT(QT_BAR_SHARED),
      "hc-consumer");
  if (ret)
    return ret;

  self->regs = pcim_iomap_table(owner)[QT_BAR_REGS];
  self->msix = pcim_iomap_table(owner)[QT_BAR_MSIX];
  self->shared = pcim_iomap_table(owner)[QT_BAR_SHARED];
  self->shared_size = pci_resource_len(owner, QT_BAR_SHARED);

  if (self->shared_size < offsetof(SPSCQueue, buffer))
    return -EINVAL;
  self->queue = (SPSCQueue *)self->shared;

  dev_info(&owner->dev, "BAR0=%p BAR1=%p BAR2=%p shared_size=%llu\n",
           self->regs, self->msix, self->shared,
           (unsigned long long)self->shared_size);

  ret = pci_alloc_irq_vectors(owner, 1, 1, PCI_IRQ_MSIX);
  if (ret < 0)
    return ret;

  self->irq = pci_irq_vector(owner, 0);

  ret = devm_request_irq(&owner->dev, self->irq, hc_irq, 0,
                         "linux_hc_transport", self);
  if (ret)
    goto err_irq_vectors;

  dev_info(&owner->dev, "IRQ=%d\n", self->irq);

  return 0;

err_irq_vectors:
  pci_free_irq_vectors(owner);
  return ret;
}

static void linux_hc_transport_destroy(LinuxHCTransport *self) {
  pci_free_irq_vectors(self->owner);

  // Helps debugging.
  *self = (LinuxHCTransport){0};
}

static long hc_producer_run(struct hc_producer_device *hc) {
  const ProducerInfo info = {
      .transport = LinuxHCTransportAsTransportMut(&hc->transport),
      .runtime = LinuxRuntimeAsRuntimeMut(NULL),
      .work_cost = 3000,
  };
  return run_producer(&info);
}

static long hc_producer_ioctl(struct file *file, unsigned int cmd,
                              unsigned long arg) {
  int ret;
  struct hc_producer_device *hc = file->private_data;

  switch (cmd) {
  case HCP_IOCTL_RUN:
    if (!mutex_trylock(&hc->run_lock))
      return -EBUSY;

    ret = hc_producer_run(hc);
    mutex_unlock(&hc->run_lock);
    return ret;

  default:
    return -ENOTTY;
  }
}

static int hc_producer_open(struct inode *inode, struct file *file) {
  struct miscdevice *miscdev = file->private_data;
  struct hc_producer_device *hc;

  hc = container_of(miscdev, struct hc_producer_device, miscdev);

  file->private_data = hc;

  return 0;
}

static const struct file_operations hc_fops = {
    .owner = THIS_MODULE,
    .open = hc_producer_open,
    .unlocked_ioctl = hc_producer_ioctl,
#ifdef CONFIG_COMPAT
    .compat_ioctl = hc_producer_ioctl,
#endif
};

static int hc_producer_probe(struct pci_dev *pdev,
                             const struct pci_device_id *id) {
  int ret;
  struct hc_producer_device *hc;

  hc = devm_kzalloc(&pdev->dev, sizeof(*hc), GFP_KERNEL);
  if (!hc) {
    ret = -ENOMEM;
    goto err;
  }

  mutex_init(&hc->run_lock);
  pci_set_drvdata(pdev, hc);

  ret = linux_hc_transport_init(&hc->transport, pdev);
  if (ret != 0) {
    goto err;
  }

  hc->miscdev.minor = MISC_DYNAMIC_MINOR;
  hc->miscdev.name = "hc-producer";
  hc->miscdev.fops = &hc_fops;
  hc->miscdev.parent = &pdev->dev;

  ret = misc_register(&hc->miscdev);
  if (ret)
    goto err_destroy_transport;

  return 0;

err_destroy_transport:
  linux_hc_transport_destroy(&hc->transport);
err:
  return ret;
}

static void hc_producer_remove(struct pci_dev *pdev) {
  struct hc_producer_device *hc = pci_get_drvdata(pdev);

  misc_deregister(&hc->miscdev);

  mutex_lock(&hc->run_lock);

  linux_hc_transport_destroy(&hc->transport);

  mutex_unlock(&hc->run_lock);

  dev_info(&pdev->dev, "removed\n");
}

static struct pci_driver hc_producer_driver = {
    .name = "hc_producer",
    .id_table = hc_ids,
    .probe = hc_producer_probe,
    .remove = hc_producer_remove,

};

module_pci_driver(hc_producer_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION(
    "Implements the producer side of the hypercall-based SPSC loop");
