#include "host.h"
#include "common.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "contrib/producer-consumer/core/transport.h"
#include "contrib/producer-consumer/core/spsc_queue.h"

typedef struct IvshmemHost {
  const char *shm_name;
  uint64_t shm_size;
  int shm_fd;

  const char *server_path;
  int server_fd;

  int client_fd;
  SPSCQueue *queue;
} IvshmemHost;

static SPSCQueue *ivshmem_host_get_queue(Transport self) {
  return ((IvshmemHost *)self.data)->queue;
}

static int ivshmem_host_notify(Transport self) { return 0; }

static int ivshmem_host_wait_until(Transport self) { return 0; }

static void ivshmem_host_destroy(Transport self) {
  IvshmemHost *data = self.data;

  close(data->client_fd);
  unlink(data->server_path);
  close(data->server_fd);
  munmap(data->queue, data->shm_size);
  close(data->shm_fd);
  shm_unlink(data->shm_name);
  free(data);
}

static TransportOps ivshmem_host_ops = {
    .get_queue = ivshmem_host_get_queue,
    .notify = ivshmem_host_notify,
    .wait_until = ivshmem_host_wait_until,
    .destroy = ivshmem_host_destroy,
};

int ivshmem_host_create(const IvshmemHostCreateInfo *info, Transport *output) {
  int res;
  IvshmemHost *data;

  // Allocate data
  data = calloc(1, sizeof(*data));
  if (data == NULL) {
    perror("calloc");
    res = ENOMEM;
    goto err;
  }
  data->server_path = info->socket_path;
  data->shm_name = info->shm_name;
  data->shm_size = info->shm_size;

  // Setup shared memory queue
  data->shm_fd = shm_open(info->shm_name, O_CREAT | O_RDWR, S_IRWXU);
  if (data->shm_fd == -1) {
    perror("shm_open");
    res = errno;
    goto err_free_data;
  }

  if (ftruncate(data->shm_fd, info->shm_size) != 0) {
    perror("ftruncate");
    res = errno;
    goto err_close_shm;
  }

  data->queue = mmap(NULL, info->shm_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                     data->shm_fd, 0);
  if (data->queue == MAP_FAILED) {
    perror("mmap");
    res = errno;
    goto err_close_shm;
  }

  spsc_queue_init(data->queue, info->shm_size);

  // Setup server socket
  data->server_fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
  if (data->server_fd == -1) {
    perror("socket");
    res = errno;
    goto err_munmap;
  }

  struct sockaddr_un addr = {0};
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, data->server_path, sizeof(addr.sun_path) - 1);

  if (bind(data->server_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    perror("bind");
    res = errno;
    goto err_close_socket;
  }

  if (listen(data->server_fd, 1) != 0) {
    perror("listen");
    res = errno;
    goto err_unlink_socket;
  }

  data->client_fd = accept(data->server_fd, NULL, NULL);
  if (data->client_fd == -1) {
    perror("accept");
    res = errno;
    goto err_unlink_socket;
  }

  // Send setup message
  IvshmemCMsgSetupWire setup_msg = {
      .header =
          {
              .kind = IVSHMEM_CMSG_SETUP,
              .length = sizeof(IvshmemCMsgSetupWire),
          },
      .version = 0,
      .shm_size = info->shm_size,
  };
  struct iovec iov = {
      .iov_base = &setup_msg,
      .iov_len = sizeof(setup_msg),
  };

  int fds[1] = {data->shm_fd};
  char control[CMSG_SPACE(sizeof(fds))] = {0};

  struct msghdr msg = {0};
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = control;
  msg.msg_controllen = sizeof(control);

  struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
  cmsg->cmsg_level = SOL_SOCKET;
  cmsg->cmsg_type = SCM_RIGHTS;
  cmsg->cmsg_len = CMSG_LEN(sizeof(data->shm_fd));

  memcpy(CMSG_DATA(cmsg), &fds, sizeof(fds));

  if (sendmsg(data->client_fd, &msg, 0) == -1) {
    perror("sendmsg");
    res = errno;
    goto err_close_client;
  }

  // Finish filling output
  output->ops = &ivshmem_host_ops;
  output->data = data;
  return 0;

err_close_client:
  close(data->client_fd);

err_unlink_socket:
  unlink(data->server_path);

err_close_socket:
  close(data->server_fd);

err_munmap:
  munmap(data->queue, info->shm_size);

err_close_shm:
  close(data->shm_fd);
  shm_unlink(info->shm_name);

err_free_data:
  free(data);

err:
  return res;
}
