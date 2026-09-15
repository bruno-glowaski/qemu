#ifndef PC_CORE_PRODUCER_H
#define PC_CORE_PRODUCER_H

#include "common.h"
#include "measurement.h"
#include "spsc_queue.h"
#include "portable.h"
#include "transport.h"

typedef struct {
  tsc_t work_cost;
} ProducerInfo;

static inline int run_producer(const ProducerInfo *info, Transport transport,
                               PerPacketEvents *events, uint64_t events_len) {
  uint64_t event_idx = 0;
  int res = 0;
  SPSCQueue *queue;
  Packet *packet;
  tsc_t work_cost, wait_start, wait_end, work_start, work_end, signal_start = 0,
                                                               signal_end = 0;

  work_cost = info->work_cost;
  queue = transport.ops->get_queue(transport);
  for (;;) {
    if (event_idx > events_len) {
      res = CREND;
      goto end;
    }
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

    work_start = read_tsc();
    while (read_tsc() - wait_start < work_cost) {
      barrier();
    }
    work_end = read_tsc();

    events[event_idx].consumer = packet->consumer_events;
    events[event_idx].producer = (PerSideEvents){
        .wait_start = wait_start,
        .wait_end = wait_end,
        .work_start = work_start,
        .work_end = work_end,
        .signal_start = signal_start,
        .signal_end = signal_end,
    };

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

    event_idx++;
  }

end:
  return res;
}

#endif // !PC_CORE_PRODUCER_H
