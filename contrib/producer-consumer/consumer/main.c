#include <fcntl.h>
#include <inttypes.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "contrib/producer-consumer/core/portable.h"
#include "contrib/producer-consumer/core/transport.h"
#include "contrib/producer-consumer/core/spsc_queue.h"

#include "contrib/producer-consumer/ivshmem/host.h"

static IvshmemHostCreateInfo ivshmem_create_info = {
    .server_path = "/tmp/ivshmem_server.sock",
};

int main(void) {
  Transport transport;
  SPSCQueue *queue;
  int res;
  Packet *packet;
  char buf[512];
  tsc_t consumer_work = 5000;

  res = ivshmem_host_create(&ivshmem_create_info, &transport);
  if (res != 0) {
    perror("ivshmem_host_create");
    return res;
  }

  if (fix_hart() != 1) {
    perror("fix_hart");
    return -1;
  }
  queue = transport.ops->get_queue(transport);
  for (;;) {
    if (transport.ops->is_closed(transport)) {
      printf("transport broken\n");
      goto end;
    }

    packet = spsc_queue_peek_pop(queue);
    while (packet == NULL) {
      res = transport.ops->wait_until(transport);
      if (res != 0) {
        perror("wait_until");
        goto end;
      }
      packet = spsc_queue_peek_pop(queue);
    }

    packet->cons_work_start = read_tsc();
    work_for(consumer_work);
    packet->cons_work_end = read_tsc();

    int len = snprintf(buf, sizeof(buf),
                       "PACKET: Ps: %" PRIu64 "; Pe: %" PRIu64 "; Cs: %" PRIu64
                       "; Ce: %" PRIu64 ";\n",
                       packet->prod_work_start, packet->prod_work_end,
                       packet->cons_work_start, packet->cons_work_end);

    write(STDOUT_FILENO, buf, (size_t)len);

    spsc_queue_commit_pop(queue);
  }

end:
  fflush(stdout);
  transport.ops->destroy(transport);
  return res;
}
