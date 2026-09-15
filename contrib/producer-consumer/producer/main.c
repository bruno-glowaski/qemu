#include <stdio.h>

#include "contrib/producer-consumer/core/transport.h"
#include "contrib/producer-consumer/core/producer.h"

#include "contrib/producer-consumer/ivshmem/guest.h"

static IvshmemGuestCreateInfo ivshmem_create_info = {};
static ProducerInfo producer_info = {.work_cost = 5000};

int main(void) {
  Transport transport;
  int res;

  res = ivshmem_guest_create(&ivshmem_create_info, &transport);
  if (res != 0) {
    perror("ivshmem_guest_create");
    return res;
  }

  res = run_producer(&producer_info, transport);
  transport.ops->destroy(transport);

  return res;
}
