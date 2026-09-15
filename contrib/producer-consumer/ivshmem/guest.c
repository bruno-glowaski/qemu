#include "client.h"
#include "common.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "contrib/producer-consumer/core/transport.h"
#include "contrib/producer-consumer/core/spsc_queue.h"

typedef struct IvshmemClientTransport {
  uint64_t shm_size;
  int shm_fd;

  int server_fd;

  SPSCQueue *queue;
} IvshmemClientTransport;

static SPSCQueue *ivshmem_client_transport_get_queue(Transport self) {
  return ((IvshmemClientTransport *)self.data)->queue;
}

static int ivshmem_client_transport_notify(Transport self) { return 0; }

static int ivshmem_client_transport_wait_until(Transport self) { return 0; }

static int ivshmem_client_transport_is_closed(Transport self) {
  IvshmemClientTransport *data = self.data;

  char byte;
  ssize_t n =
      recv(data->server_fd, &byte, sizeof(byte), MSG_PEEK | MSG_DONTWAIT);

  if (n == 0)
    return 1;

  if (n < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK)
      return 0;

    return 1;
  }

  return 0;
}

static void ivshmem_client_transport_destroy(Transport self) {
  IvshmemClientTransport *data = self.data;

  munmap(data->queue, data->shm_size);
  close(data->shm_fd);
  close(data->server_fd);

  free(data);
}

static TransportOps ivshmem_client_transport_ops = {
    .get_queue = ivshmem_client_transport_get_queue,
    .notify = ivshmem_client_transport_notify,
    .wait_until = ivshmem_client_transport_wait_until,
    .is_closed = ivshmem_client_transport_is_closed,
    .destroy = ivshmem_client_transport_destroy,
};

int ivshmem_client_transport_create(const IvshmemClientCreateInfo *info,
                                    Transport *output) {
  int res;
  IvshmemClientTransport *data = NULL;

  data = calloc(1, sizeof(*data));
  if (data == NULL) {
    perror("calloc");
    return ENOMEM;
  }

  data->server_fd = -1;
  data->shm_fd = -1;
  data->queue = MAP_FAILED;

  /*
   * Connect to the server's Unix socket.
   */
  data->server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (data->server_fd == -1) {
    perror("socket");
    res = errno;
    goto err_free_data;
  }

  struct sockaddr_un addr = {0};
  addr.sun_family = AF_UNIX;

  if (strlen(info->socket_path) >= sizeof(addr.sun_path)) {
    res = ENAMETOOLONG;
    goto err_close_socket;
  }

  strcpy(addr.sun_path, info->socket_path);

  if (connect(data->server_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    perror("connect");
    res = errno;
    goto err_close_socket;
  }

  /*
   * Receive the setup message and the shared-memory file descriptor.
   */
  char setup[sizeof("HELLO")];

  char control[CMSG_SPACE(sizeof(int))];

  struct iovec iov = {
      .iov_base = setup,
      .iov_len = sizeof(setup),
  };

  struct msghdr msg = {
      .msg_name = NULL,
      .msg_namelen = 0,
      .msg_flags = 0,
      .msg_iov = &iov,
      .msg_iovlen = 1,
      .msg_control = control,
      .msg_controllen = sizeof(control),
  };

  ssize_t n = recvmsg(data->server_fd, &msg, 0);
  if (n < 0) {
    perror("recvmsg");
    res = errno;
    goto err_close_socket;
  }

  if (n == 0) {
    fprintf(stderr, "server closed connection during setup\n");
    res = ECONNRESET;
    goto err_close_socket;
  }

  if (msg.msg_flags & MSG_CTRUNC) {
    fprintf(stderr, "recvmsg: control message truncated\n");
    res = EPROTO;
    goto err_close_socket;
  }

  if ((size_t)n != sizeof("HELLO") ||
      memcmp(setup, "HELLO", sizeof("HELLO")) != 0) {
    fprintf(stderr, "invalid server setup message\n");
    res = EPROTO;
    goto err_close_socket;
  }

  /*
   * Find the SCM_RIGHTS message.
   */
  struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);

  if (cmsg == NULL || cmsg->cmsg_level != SOL_SOCKET ||
      cmsg->cmsg_type != SCM_RIGHTS ||
      cmsg->cmsg_len != CMSG_LEN(sizeof(int))) {
    fprintf(stderr, "server did not send shared-memory fd\n");
    res = EPROTO;
    goto err_close_socket;
  }

  memcpy(&data->shm_fd, CMSG_DATA(cmsg), sizeof(data->shm_fd));

  /*
   * Map the shared memory.
   */
  data->queue = mmap(NULL, data->shm_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                     data->shm_fd, 0);

  if (data->queue == MAP_FAILED) {
    perror("mmap");
    res = errno;
    goto err_close_shm;
  }

  output->ops = &ivshmem_client_transport_ops;
  output->data = data;

  return 0;

err_close_shm:
  close(data->shm_fd);

err_close_socket:
  close(data->server_fd);

err_free_data:
  free(data);

  return res;
}
