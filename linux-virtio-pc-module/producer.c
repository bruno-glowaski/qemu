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
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/virtio.h>
#include <linux/virtio_config.h>
#include <linux/wait.h>

#include "virtio-pc.h"

#define VIRTIO_ID_PC_PRODUCER 74
#define WORK_QUEUE_NAME "work_queue"

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
  struct pcbuf *buf;
  unsigned int len;

  buf = virtqueue_get_buf(pc->vq, &len);
  printk("virtio-pc-producer: cleaned up buf %p\n", buf);
}

static void do_work(uint64_t cost) {
  uint64_t ts_end = rdtsc() + cost;
  while (ts_end < rdtsc())
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
  DECLARE_WAITQUEUE(queue_empty, current);

  vq = pc->vq;
  wq_buf = pc->wq_buf;
  work_cost = config->work_cost_production;

  add_wait_queue(&pc->wq_empty_wqh, &queue_empty);
  for (;;) {
    if (unlikely(signal_pending(current))) {
      printk("virtio-pc-producer: signal received, returning\n");
      ret = -EAGAIN;
      goto end;
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
    }

    if (vq->num_free == 0) {
      set_current_state(TASK_INTERRUPTIBLE);
      if (!virtqueue_enable_cb_delayed(vq)) {
        cleanup_items();
      }
      if (vq->num_free > 0) {
        virtqueue_disable_cb(vq);
        set_current_state(TASK_RUNNING);
      } else {
        schedule();
      }
    }
  }

end:
  remove_wait_queue(&pc->wq_empty_wqh, &queue_empty);
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

  pc = kzalloc(sizeof(*pc), GFP_KERNEL);
  if (!pc) {
    ret = -ENOMEM;
    goto end;
  }
  pc->vdev = vdev;
  vdev->priv = pc;

  pc->wq_buf =
      kzalloc(config->work_queue_len * sizeof(struct virtio_pc_wu), GFP_KERNEL);
  if (!pc->wq_buf) {
    ret = -ENOMEM;
    goto cleanup_pc;
  }

  pc->vq = virtio_find_single_vq(vdev, virtio_pc_producer_on_queue_notify,
                                 WORK_QUEUE_NAME);
  if (IS_ERR(pc->vq)) {
    ret = PTR_ERR(pc->vq);
    goto cleanup_pc;
  }

  misc_register(&virtio_pc_producer_misc);

  goto end;

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

  mutex_lock(&global_lock);

  if (!virtio_has_feature(vdev, VIRTIO_F_VERSION_1)) {
    ret = -1;
    goto end;
  }

  if (pc != NULL) {
    ret = -1;
    goto end;
  }

  ret = virtio_pc_read_config();
  if (ret != 0) {
    goto end;
  }

  ret = virtio_pc_producer_init(vdev);
  if (ret != 0) {
    goto cleanup_config;
  }

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

static unsigned int features[] = {/* empty */};

static const struct virtio_device_id id_table[] = {
    {VIRTIO_ID_PC_PRODUCER, VIRTIO_DEV_ANY_ID}, {0}};

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
