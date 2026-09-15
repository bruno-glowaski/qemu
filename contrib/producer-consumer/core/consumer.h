#ifndef PC_CORE_CONSUMER_H
#define PC_CORE_CONSUMER_H

#include "common.h"
#include "measurement.h"
#include "spsc_queue.h"
#include "portable.h"
#include "transport.h"

typedef struct {
  tsc_t work_cost;
} ConsumerInfo;

static inline int run_consumer(const ConsumerInfo *info, Transport transport) {
  int res = 0;
  SPSCQueue *queue;
  Packet *packet;
  tsc_t work_cost, wait_start, wait_end, signal_start = 0, signal_end = 0;

  work_cost = info->work_cost;
  queue = transport.ops->get_queue(transport);
  for (;;) {
    if (transport.ops->is_closed(transport)) {
      res = CRCLOSED;
      goto end;
    }

    packet = spsc_queue_peek_push(queue);
    if (packet == NULL) {
      do {
        wait_start = read_tsc();
        res = transport.ops->wait_until(transport);
        wait_end = read_tsc();
        if (res != 0) {
          goto end;
        }
        packet = spsc_queue_peek_push(queue);
      } while (packet == NULL);
    } else {
      wait_start = 0;
      wait_end = 0;
    }

    packet->consumer_events.work_start = read_tsc();
    packet->consumer_events.wait_start = wait_start;
    packet->consumer_events.wait_end = wait_end;
    packet->consumer_events.signal_start = signal_start;
    packet->consumer_events.signal_end = signal_end;
    while (read_tsc() - wait_start < work_cost) {
      barrier();
    }
    packet->consumer_events.work_end = read_tsc();
    spsc_queue_commit_push(queue);

    if (spsc_queue_consumer_needs_signal(queue)) {
      signal_start = read_tsc();
      res = transport.ops->notify(transport);
      if (res != 0) {
        goto end;
      }
      signal_end = read_tsc();
    } else {
      signal_start = 0;
      signal_end = 0;
    }
  }

end:
  return res;
}

#endif // !PC_CORE_CONSUMER_H
