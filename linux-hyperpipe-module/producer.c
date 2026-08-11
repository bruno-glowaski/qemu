#include <asm-generic/errno-base.h>
#include <linux/array_size.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/gfp_types.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/virtio_config.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/virtio.h>
#include <linux/virtio_config.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <uapi/linux/virtio_ids.h>
#include <linux/wait.h>

#include "linux/mutex.h"
#include "virtio-echo.h"

#define VIRTIO_ID_ECHO 74
#define WORK_QUEUE_NAME "work_queue"

/* Single work unit */
struct virtio_pc_wu {
  uint64_t produced_at;
};

/*
 *
 * VirtIO produce-consume loop device private info
 *
 * `wq` stands for work queue
 *
 * */
struct virtio_pc_device {
  struct virtio_device *vdev;
  struct virtqueue *vq;

  uint32_t wq_len;
  wait_queue_head_t wq_empty_wqh;
  struct scatterlist wq_sg[1];
  struct virtio_pc_wu *wq_buf;
};

struct virtio_pc_config {
  uint64_t work_cost_production;
};

/* global lock protected */
static struct virtio_pc_device *pc = NULL;
DEFINE_MUTEX(global_lock);

static long virtio_echo_run_producer(void) {
  int ret = 0;
  DECLARE_WAITQUEUE(queue_empty, current);
  add_wait_queue(&pc->wq_empty_wqh, &queue_empty);

  for (;;) {
  }

end:
  remove_wait_queue(&pc->wq_empty_wqh, &queue_empty);
  return ret;
}

static long virtio_pc_ioctl(struct file *f, unsigned int cmd,
                            unsigned long arg) {
  switch (cmd) {
  case VIRTIO_ECHO_CMD_START:
    return virtio_echo_run_producer();
  default:
    return -EINVAL;
  }
}

static struct file_operations virtio_pc_fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = virtio_pc_ioctl,
    .llseek = noop_llseek,
};

static struct miscdevice virtio_pc_misc = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = "virtio-pc",
    .fops = &virtio_pc_fops,
};

static void virtio_echo_producer_on_queue_notify(struct virtqueue *vq) {
  struct virtio_pc_device *priv = vq->vdev->priv;

  virtqueue_disable_cb(vq);
  wake_up_interruptible(&priv->wq_empty_wqh);
}

static int virtio_echo_probe(struct virtio_device *vdev) {
  int ret = 0;

  mutex_lock(&global_lock);

  // Check for VirtIO V1
  if (!virtio_has_feature(vdev, VIRTIO_F_VERSION_1)) {
    ret = -1;
    goto end;
  }

  // Only 1 device is allowed
  if (pc != NULL) {
    ret = -1;
    goto end;
  }

  // Allocate device
  pc = kzalloc(sizeof(*pc), GFP_KERNEL);
  if (!pc) {
    ret = -ENOMEM;
    goto end;
  }
  pc->vdev = vdev;
  vdev->priv = pc;

  // Allocate queue buffer
  pc->wq_len = 512; // Default queue length = 512
  pc->wq_buf = kzalloc(pc->wq_len * sizeof(struct virtio_pc_wu), GFP_KERNEL);
  if (!pc->wq_buf) {
    ret = -ENOMEM;
    goto cleanup_pc;
  }

  // Initialize scatterlist
  sg_init_one(pc->wq_sg, pc->wq_buf, pc->wq_len);

  // Find work VirtQueue
  pc->vq = virtio_find_single_vq(vdev, virtio_echo_producer_on_queue_notify,
                                 WORK_QUEUE_NAME);
  if (IS_ERR(pc->vq)) {
    ret = PTR_ERR(pc->vq);
    goto cleanup_pc;
  }

  // Register device
  misc_register(&virtio_pc_misc);

  goto end;

cleanup_pc:
  kfree(pc);
  pc = NULL;
end:
  mutex_unlock(&global_lock);
  return ret;
}

static void virtio_echo_remove(struct virtio_device *vdev) {
  mutex_lock(&global_lock);

  if (pc == NULL)
    goto end;

  misc_deregister(&virtio_pc_misc);

end:
  mutex_unlock(&global_lock);
}

static unsigned int features[] = {/* empty */};

static const struct virtio_device_id id_table[] = {
    {VIRTIO_ID_ECHO, VIRTIO_DEV_ANY_ID}, {0}};

static struct virtio_driver virtio_echo_driver = {
    .driver.name = KBUILD_MODNAME,
    .feature_table = features,
    .feature_table_size = ARRAY_SIZE(features),
    .id_table = id_table,
    .probe = virtio_echo_probe,
    .remove = virtio_echo_remove,
};

module_virtio_driver(virtio_echo_driver);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Bruno Henrique Glowaski Morais");
MODULE_DESCRIPTION("Implements the producer side of virtio_echo");
MODULE_VERSION("0.1");
