#include <asm/tsc.h>

#include <linux/compiler.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/printk.h>
#include <linux/scatterlist.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/virtio.h>
#include <linux/virtio_config.h>
#include <linux/wait.h>

#include "linux/virtio_config.h"
#include "virtio-pc.h"

/* Single work unit */
struct virtio_pc_wu {
  uint64_t prod_start_at;
  uint64_t prod_end_at;
};

/*
 *
 * VirtIO produce-consume loop device private info
 *
 * `wq` stands for work queue
 *
 * */
struct virtio_pc_producer {
  struct virtio_device *vdev;
  struct virtqueue *vq;

  wait_queue_head_t wq_empty_wqh;
  struct virtio_pc_wu *wq_buf;
};

struct virtio_pc_config {
  uint32_t work_queue_len;
  uint64_t work_cost_production;
};

/* global lock protected */
static struct virtio_pc_producer *pc = NULL;
static struct virtio_pc_config *config = NULL;
DEFINE_MUTEX(global_lock);

static void cleanup_items(void) {
  struct virtio_pc_wu *buf;
  unsigned int len;

  while ((buf = virtqueue_get_buf(pc->vq, &len)) != NULL) {
    printk("virtio-pc-producer: cleaned up buf %p\n", buf);
  }
}

static void do_work(uint64_t cost) {
  uint64_t ts_end = rdtsc() + cost;
  while (rdtsc() < ts_end)
    barrier();
}

static long virtio_pc_producer_main_thread(void) {
  int ret = 0, err = 0;
  bool kick = 0;
  size_t idx = 0;
  uint64_t work_cost = 3000;
  struct virtqueue *vq = NULL;
  struct virtio_pc_wu *wq_buf = NULL;
  struct virtio_pc_wu *pkg = NULL;
  struct scatterlist sg;

  vq = pc->vq;
  wq_buf = pc->wq_buf;
  work_cost = config->work_cost_production;

  for (;;) {
    if (unlikely(signal_pending(current))) {
      printk("virtio-pc-producer: signal received, returning\n");
      ret = -EAGAIN;
      goto end;
    }

    cleanup_items();
    if (vq->num_free == 0) {
      virtqueue_enable_cb(vq);
      cleanup_items();

      wait_event_interruptible(pc->wq_empty_wqh, ({
                                 cleanup_items();
                                 vq->num_free > 0 || signal_pending(current);
                               }));

      virtqueue_disable_cb(vq);

      if (vq->num_free == 0)
        continue;
    }

    pkg = &wq_buf[idx];
    pkg->prod_start_at = rdtsc();
    do_work(work_cost);
    pkg->prod_end_at = rdtsc();

    sg_init_one(&sg, pkg, sizeof(*pkg));
    err = virtqueue_add_outbuf(vq, &sg, 1, pkg, GFP_ATOMIC);

    kick = virtqueue_kick_prepare(vq);
    if (kick) {
      virtqueue_notify(vq);
    }

    if (unlikely(err)) {
      printk("virtio-pc-producer: virtio_add_outbuf() failed with %d\n", err);
    } else {
      printk("virtio-pc-producer: produced pkg %p\n", pkg);
      idx = (idx + 1) % config->work_queue_len;
    }
  }

end:
  return ret;
}

static long virtio_pc_producer_ioctl(struct file *f, unsigned int cmd,
                                     unsigned long arg) {
  switch (cmd) {
  case VIRTIO_PC_CMD_START:
    return virtio_pc_producer_main_thread();
  default:
    return -EINVAL;
  }
}

static struct file_operations virtio_pc_producer_fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = virtio_pc_producer_ioctl,
    .llseek = noop_llseek,
};

static struct miscdevice virtio_pc_producer_misc = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = "virtio-pc-producer",
    .fops = &virtio_pc_producer_fops,
};

static void virtio_pc_producer_on_queue_notify(struct virtqueue *vq) {
  struct virtio_pc_producer *priv = vq->vdev->priv;

  virtqueue_disable_cb(vq);
  wake_up_interruptible(&priv->wq_empty_wqh);
}

static int virtio_pc_read_config(void) {
  config = kzalloc(sizeof(*config), GFP_KERNEL);
  if (config == NULL) {
    return -ENOMEM;
  }

  config->work_queue_len = 512;
  config->work_cost_production = 3000;

  return 0;
}

static void virtio_pc_free_config(void) {
  kfree(config);
  config = NULL;
}

static int virtio_pc_producer_init(struct virtio_device *vdev) {
  int ret = 0;

  printk("virtio-pc-producer: allocating producer...\n");
  pc = kzalloc(sizeof(*pc), GFP_KERNEL);
  if (!pc) {
    printk("virtio-pc-producer: failed to allocate!\n");
    ret = -ENOMEM;
    goto end;
  }
  pc->vdev = vdev;
  init_waitqueue_head(&pc->wq_empty_wqh);
  vdev->priv = pc;

  printk("virtio-pc-producer: allocating work queue buffers...\n");
  pc->wq_buf =
      kzalloc(config->work_queue_len * sizeof(struct virtio_pc_wu), GFP_KERNEL);
  if (!pc->wq_buf) {
    printk("virtio-pc-producer: failed to allocate!\n");
    ret = -ENOMEM;
    goto cleanup_pc;
  }

  printk("virtio-pc-producer: searching for work queue...\n");
  pc->vq = virtio_find_single_vq(vdev, virtio_pc_producer_on_queue_notify,
                                 WORK_QUEUE_NAME);
  if (IS_ERR(pc->vq)) {
    printk("virtio-pc-producer: failed to get work queue: %ld\n",
           PTR_ERR(pc->vq));
    ret = PTR_ERR(pc->vq);
    goto cleanup_wq_buf;
  }

  printk("virtio-pc-producer: adding misc device...\n");
  misc_register(&virtio_pc_producer_misc);

  goto end;

cleanup_wq_buf:
  kfree(pc->wq_buf);
cleanup_pc:
  kfree(pc);
  pc = NULL;
end:
  return ret;
}

static void virtio_pc_producer_destroy(void) {
  misc_deregister(&virtio_pc_producer_misc);

  kfree(pc->wq_buf);

  kfree(pc);

  pc = NULL;
}

static int virtio_pc_probe(struct virtio_device *vdev) {
  int ret = 0;

  printk("virtio-pc-producer: acquiring global lock...\n");
  mutex_lock(&global_lock);

  printk("virtio-pc-producer: checking virtio v1 support...\n");
  if (!virtio_has_feature(vdev, VIRTIO_F_VERSION_1)) {
    printk("virtio-pc-producer: no virtio v1 support!\n");
    ret = -1;
    goto end;
  }

  printk("virtio-pc-producer: checking if producer has already been "
         "initialized...\n");
  if (pc != NULL) {
    printk("virtio-pc-producer: producer has already been initialized. "
           "skipping...\n");
    ret = -1;
    goto end;
  }

  printk("virtio-pc-producer: reading config...\n");
  ret = virtio_pc_read_config();
  if (ret != 0) {
    printk("virtio-pc-producer: failed to read config!\n");
    goto end;
  }

  printk("virtio-pc-producer: initializing...\n");
  ret = virtio_pc_producer_init(vdev);
  if (ret != 0) {
    printk("virtio-pc-producer: failed to initialize producer!\n");
    goto cleanup_config;
  }

  printk("virtio-pc-producer: probe done!\n");
  goto end;

cleanup_config:
  virtio_pc_free_config();
end:
  mutex_unlock(&global_lock);
  return ret;
}

static void virtio_pc_remove(struct virtio_device *vdev) {
  mutex_lock(&global_lock);

  if (pc == NULL)
    goto end;

  virtio_pc_producer_destroy();
  virtio_pc_free_config();

end:
  mutex_unlock(&global_lock);
}

static unsigned int features[] = {VIRTIO_F_VERSION_1};

static const struct virtio_device_id id_table[] = {
    {VIRTIO_ID_PC, VIRTIO_DEV_ANY_ID}, {0}};

static struct virtio_driver virtio_pc_driver = {
    .driver.name = KBUILD_MODNAME,
    .feature_table = features,
    .feature_table_size = ARRAY_SIZE(features),
    .id_table = id_table,
    .probe = virtio_pc_probe,
    .remove = virtio_pc_remove,
};

module_virtio_driver(virtio_pc_driver);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Bruno Henrique Glowaski Morais");
MODULE_DESCRIPTION("Implements the producer side of virtio_pc");
MODULE_VERSION("0.1");
