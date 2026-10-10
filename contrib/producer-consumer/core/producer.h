#ifndef PC_CORE_PRODUCER_H
#define PC_CORE_PRODUCER_H

#include "common.h"
#include "measurement.h"
#include "spsc_queue.h"
#include "portable.h"
#include "transport.h"

typedef struct {
  Costs costs;

  RuntimeMut runtime;
  TransportMut transport;
} ProducerInfo;

static inline int run_producer(const ProducerInfo *info) {
  int res = 0;
  SPSCQueue *queue;
  Packet *packet;
  Costs costs;
  tsc_t yield_start = 0, yield_end = 0, resume_start = 0, resume_end = 0,
        work_start = 0, work_end = 0, notify_start = 0, notify_end = 0;
  RuntimeMut runtime = info->runtime;
  TransportMut transport = info->transport;

  costs = info->costs;
  queue = transport.ops->get_queue(AsTransportConst(transport));

  runtime.ops->fix_hart(runtime);

  res = transport.ops->ready_barrier_wait(transport);
  if (res != 0) {
    goto end;
  }

  for (;;) {
    if (runtime.ops->is_interrupted(AsRuntimeConst(runtime))) {
      res = CRINTERRUPTED;
      goto end;
    }
    if (transport.ops->is_closed(AsTransportConst(transport))) {
      res = CRCLOSED;
      goto end;
    }

    packet = spsc_queue_peek_push(queue);
    if (packet == NULL) {
      do {
        yield_start = read_tsc();
        while ((yield_end = read_tsc()) - yield_start < costs.yield) {
          mbarrier();
        }

        spsc_queue_request_signal_for_producer(queue);

        // A packet might have been published between the prior peek and
        // request_signal.
        packet = spsc_queue_peek_push(queue);
        if (packet != NULL) {
          break;
        }

        res = transport.ops->wait_until(transport);
        if (res != 0) {
          goto end;
        }

        resume_start = read_tsc();
        while ((resume_end = read_tsc()) - resume_start < costs.resume) {
          mbarrier();
        }

        packet = spsc_queue_peek_push(queue);
      } while (packet == NULL);
      spsc_queue_clear_signal_for_producer(queue);
    } else {
      resume_start = 0;
      resume_end = 0;
      yield_start = 0;
      yield_end = 0;
    }

    work_start = read_tsc();
    while ((work_end = read_tsc()) - work_start < costs.work) {
      mbarrier();
    }

    packet->consumer_events.work_start = work_start;
    packet->consumer_events.work_end = work_end;
    packet->consumer_events.yield_start = yield_start;
    packet->consumer_events.yield_end = yield_end;
    packet->consumer_events.resume_start = resume_start;
    packet->consumer_events.resume_end = resume_end;
    packet->consumer_events.notify_start = notify_start;
    packet->consumer_events.notify_end = notify_end;

#if 0
    PC_PUSH_LOG("%ul-%ul; %ul-%Ul; %ul-%ul; %ul-%ul;",
           packet->consumer_events.work_start, packet->consumer_events.work_end,
           packet->consumer_events.yield_start,
           packet->consumer_events.yield_end,
           packet->consumer_events.resume_start,
           packet->consumer_events.resume_end,
           packet->consumer_events.notify_start,
           packet->consumer_events.notify_end);
#endif // DEBUG

    spsc_queue_commit_push(queue);

    if (spsc_queue_consumer_needs_signal(queue)) {
      notify_start = read_tsc();
      res = transport.ops->notify(transport);
      if (res != 0) {
        goto end;
      }
      while ((notify_end = read_tsc()) - notify_start < costs.notify) {
        mbarrier();
      }
    } else {
      notify_start = 0;
      notify_end = 0;
    }
  }

end:
  transport.ops->close(transport);
  return res;
}

#endif // !PC_CORE_PRODUCER_H
