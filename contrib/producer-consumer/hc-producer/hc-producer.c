#include <linux/module.h>
#include <linux/ioctl.h>
#include <linux/miscdevice.h>
#include <linux/pci.h>
#include <linux/interrupt.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <linux/wait.h>

#include "../core/portable.h"
#include "../core/transport.h"
#include "../core/producer.h"
#include "../hc-common/abi.h"
#include "../linux-kernel/common.h"

#define HCP_MODE HC_MODE_NOTIFY

struct hcp_run_config {
  __u64 yield_cost;
  __u64 resume_cost;
  __u64 work_cost;
  __u64 notify_cost;
};

#define HCP_IOCTL_RUN _IOW('H', 0, struct hcp_run_config)

static const struct pci_device_id hc_ids[] = {
    {PCI_DEVICE(HCC_VENDOR_ID, HCC_DEVICE_ID)}, {}};

MODULE_DEVICE_TABLE(pci, hc_ids);

typedef struct hc_producer_device {
  Costs costs;

  struct miscdevice miscdev;

  wait_queue_head_t waitq;

  void __iomem *regs;
  void __iomem *msix;
  // Doesn't need __iomem because this memory is guaranteed to be cacheable.
  void *shared;
  uint64_t shared_size;

  SPSCQueue *queue;

  struct msix_entry msix_entry;
  int irq;
  bool msix_enabled;

  struct mutex run_lock;
} HCProducer;

static irqreturn_t hc_irq(int irq, void *data) {
  struct hc_producer_device *prod = data;

  wake_up_interruptible(&prod->waitq);

  return IRQ_HANDLED;
}

static SPSCQueue *hc_producer_get_queue(TransportConst self) {
  const HCProducer *prod = self.data;

  return prod->queue;
}

static int hc_producer_wait_ready_barrier(TransportMut self) {
  const HCProducer *prod = self.data;
  for (;;) {
    if (readl(prod->regs + HC_REG_CONS_STATUS) != HC_SIDE_STATUS_PENDING) {
      return 0;
    }
    if (signal_pending(current)) {
      return CRINTERRUPTED;
    }
  }
}

static int hc_producer_notify(TransportMut self) {
  HCProducer *prod = self.data;

  writel(0, prod->regs + HC_REG_DOORBELL);

  return 0;
}

static bool hc_producer_is_closed(TransportConst self) {
  const HCProducer *prod = self.data;
  return readl(prod->regs + HC_REG_CONS_STATUS) == HC_SIDE_STATUS_CLOSED;
}

static int hc_producer_wait_until(TransportMut self) {
  int ret;
  HCProducer *prod = self.data;

  spsc_queue_request_signal_for_producer(prod->queue);
  ret = wait_event_interruptible(
      prod->waitq, spsc_queue_peek_push(prod->queue) != NULL ||
                       hc_producer_is_closed(AsTransportConst(self)));
  spsc_queue_clear_signal_for_producer(prod->queue);

  if (ret == -EINTR) {
    return CRINTERRUPTED;
  }

  return ret;
}

static void hc_producer_close(TransportMut self) {
  HCProducer *prod = self.data;
  writel(HC_SIDE_STATUS_CLOSED, prod->regs + HC_REG_PROD_STATUS);
}

static const TransportOps hc_producer_transport_ops = {
    .get_queue = hc_producer_get_queue,
    .ready_barrier_wait = hc_producer_wait_ready_barrier,
    .wait_until = hc_producer_wait_until,
    .notify = hc_producer_notify,
    .is_closed = hc_producer_is_closed,
    .close = hc_producer_close,
};

declare_impl(HCProducer, Transport, hc_producer_transport_ops);

static long hc_producer_run(struct hc_producer_device *prod, Costs costs) {
  const ProducerInfo info = {
      .transport = HCProducerAsTransportMut(prod),
      .runtime = LinuxRuntimeAsRuntimeMut(NULL),
      .costs = costs,
  };
  return run_producer(&info);
}

static long hc_producer_ioctl(struct file *file, unsigned int cmd,
                              unsigned long arg) {
  struct hc_producer_device *hc = file->private_data;
  struct hcp_run_config config;
  int ret;

  switch (cmd) {
  case HCP_IOCTL_RUN:
    if (copy_from_user(&config, (void __user *)arg, sizeof(config))) {
      return -EFAULT;
    }

    if (!mutex_trylock(&hc->run_lock))
      return -EBUSY;

    ret = hc_producer_run(hc, (Costs){
                                  .yield = config.yield_cost,
                                  .resume = config.resume_cost,
                                  .work = config.work_cost,
                                  .notify = config.notify_cost,
                              });
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

static const struct file_operations hc_producer_fops = {
    .owner = THIS_MODULE,
    .open = hc_producer_open,
    .unlocked_ioctl = hc_producer_ioctl,
#ifdef CONFIG_COMPAT
    .compat_ioctl = hc_producer_ioctl,
#endif
};

static int hc_producer_probe(struct pci_dev *pdev,
                             const struct pci_device_id *id) {
  struct hc_producer_device *prod;
  uint32_t hc_abi_version = 0, hc_mode = 0;
  int ret;

  prod = devm_kzalloc(&pdev->dev, sizeof(*prod), GFP_KERNEL);
  if (!prod) {
    ret = -ENOMEM;
    goto err;
  }

  pci_set_drvdata(pdev, prod);

  /*
   * Initialize struct
   */
  init_waitqueue_head(&prod->waitq);
  mutex_init(&prod->run_lock);

  /*
   * Initialize PCI
   */
  if (WARN_ON_ONCE(
          !(pci_resource_flags(pdev, HC_BAR_SHARED) & IORESOURCE_PREFETCH))) {
    dev_err(&pdev->dev, "BAR2 is not prefetchable\n");
    return -ENODEV;
  }

  ret = pcim_enable_device(pdev);
  if (ret) {
    dev_err(&pdev->dev, "failed to enable PCI device\n");
    return ret;
  }

  ret = pcim_iomap_regions(
      pdev, BIT(HC_BAR_REGS) | BIT(HC_BAR_MSIX) | BIT(HC_BAR_SHARED),
      "hc-consumer");
  if (ret) {
    dev_err(&pdev->dev, "failed to map regions\n");
    return ret;
  }

  prod->regs = pcim_iomap_table(pdev)[HC_BAR_REGS];
  prod->msix = pcim_iomap_table(pdev)[HC_BAR_MSIX];
  prod->shared = pcim_iomap_table(pdev)[HC_BAR_SHARED];
  prod->queue = (SPSCQueue *)prod->shared;
  dev_info(&pdev->dev, "BAR0=%p BAR1=%p BAR2=%p shared_size=%llu\n", prod->regs,
           prod->msix, prod->shared, prod->shared_size);

  /*
   * Validate ABI
   */

  prod->shared_size = pci_resource_len(pdev, HC_BAR_SHARED);
  if (prod->shared_size < offsetof(SPSCQueue, buffer)) {
    dev_err(&pdev->dev, "insufficient shared size (%llu)\n", prod->shared_size);
    return -EINVAL;
  }

  hc_abi_version = readl(prod->regs + HC_REG_ABI_VERSION);
  if (hc_abi_version != HC_ABI_VERSION_1_0) {
    dev_err(&pdev->dev, "ABI version mismatch (expected: %u; got: %u)\n",
            HC_ABI_VERSION_1_0, hc_abi_version);
    return -EINVAL;
  }

  hc_mode = readl(prod->regs + HC_REG_MODE);
  if (hc_mode != HCP_MODE) {
    dev_err(&pdev->dev, "waiting mode mismatch (expected: %u; got: %u)\n",
            HCP_MODE, hc_mode);
    return -EINVAL;
  }

  /*
   * Using the old MSI-X API for 4.4 compatibility.
   */
  prod->msix_entry.entry = 0;

  ret = pci_enable_msix_range(pdev, &prod->msix_entry, 1, 1);
  if (ret < 0) {
    dev_err(&pdev->dev, "failed to enable MSI-X: %d\n", ret);
    return ret;
  }

  prod->msix_enabled = true;
  prod->irq = prod->msix_entry.vector;

  ret = devm_request_irq(&pdev->dev, prod->irq, hc_irq, 0, "linux_hc_transport",
                         prod);
  if (ret) {
    dev_err(&pdev->dev, "failed to request IRQ: %d\n", ret);
    goto err_disable_msix;
  }

  dev_info(&pdev->dev, "IRQ=%d\n", prod->irq);

  prod->miscdev.minor = MISC_DYNAMIC_MINOR;
  prod->miscdev.name = "hc-producer";
  prod->miscdev.fops = &hc_producer_fops;
  prod->miscdev.parent = &pdev->dev;

  ret = misc_register(&prod->miscdev);
  if (ret) {
    dev_err(&pdev->dev, "failed to register device: %d\n", ret);

    devm_free_irq(&pdev->dev, prod->irq, prod);
    goto err_disable_msix;
  }

  return 0;

err_disable_msix:
  if (prod->msix_enabled) {
    pci_disable_msix(pdev);
    prod->msix_enabled = false;
  }
err:
  return ret;
}

static void hc_producer_remove(struct pci_dev *pdev) {
  struct hc_producer_device *prod = pci_get_drvdata(pdev);

  misc_deregister(&prod->miscdev);

  mutex_lock(&prod->run_lock);
  mutex_unlock(&prod->run_lock);

  devm_free_irq(&pdev->dev, prod->irq, prod);

  if (prod->msix_enabled) {
    pci_disable_msix(pdev);
    prod->msix_enabled = false;
  }

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
