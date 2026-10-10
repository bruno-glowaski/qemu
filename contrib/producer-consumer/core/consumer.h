#ifndef PC_CORE_CONSUMER_H
#define PC_CORE_CONSUMER_H

#include "measurement.h"
#include "spsc_queue.h"
#include "portable.h"
#include "transport.h"
#include "common.h"

typedef struct {
  PerPacketEvents *data;
  uint64_t capacity;
  uint64_t len;
} EventBuffer;

typedef struct {
  Costs costs;

  RuntimeMut runtime;
  TransportMut transport;
  EventBuffer *events;
} ConsumerInfo;

static inline void event_buffer_init(PerPacketEvents *data, uint64_t capacity,
                                     EventBuffer *events) {
  events->data = data;
  events->capacity = capacity;
  events->len = 0;
}

static inline bool event_buffer_is_filled(const EventBuffer *events) {
  return events->len >= events->capacity;
}

static inline void event_buffer_push(EventBuffer *events,
                                     PerPacketEvents data) {
  events->data[events->len++] = data;
}

static inline int run_consumer(const ConsumerInfo *info) {
  int res = 0;
  SPSCQueue *queue;
  Packet *packet;
  Costs costs;
  tsc_t yield_start = 0, yield_end = 0, resume_start = 0, resume_end = 0,
        work_start = 0, work_end = 0, notify_start = 0, notify_end = 0;
  RuntimeMut runtime = info->runtime;
  TransportMut transport = info->transport;
  EventBuffer *events = info->events;

  costs = info->costs;
  queue = transport.ops->get_queue(AsTransportConst(transport));

  runtime.ops->fix_hart(runtime);

#ifdef PC_LOG
  PC_BARRIER_START_LOG();
#endif // PC_LOG
  res = transport.ops->ready_barrier_wait(transport);
  if (res != 0) {
    goto end;
  }
#ifdef PC_LOG
  PC_BARRIER_END_LOG();
#endif // PC_LOG

  for (;;) {
    if (event_buffer_is_filled(events)) {
      res = CREND;
      goto end;
    }
    if (runtime.ops->is_interrupted(AsRuntimeConst(runtime))) {
      res = CRINTERRUPTED;
      goto end;
    }
    if (transport.ops->is_closed(AsTransportConst(transport))) {
      res = CRCLOSED;
      goto end;
    }

    packet = spsc_queue_peek_pop(queue);
    if (packet == NULL) {
      do {
        yield_start = read_tsc();
        while ((yield_end = read_tsc()) - yield_start < costs.yield) {
          mbarrier();
        }

        spsc_queue_request_signal_for_consumer(queue);

        // A packet might have been published between the prior peek and
        // request_signal.
        packet = spsc_queue_peek_pop(queue);
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

        packet = spsc_queue_peek_pop(queue);
      } while (packet == NULL);
      spsc_queue_clear_signal_for_consumer(queue);
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

#ifdef PC_LOG
    PC_POP_LOG(
        "%lu-%lu; %lu-%lu; %lu-%lu; %lu-%lu;",
        packet->consumer_events.work_start, packet->consumer_events.work_end,
        packet->consumer_events.yield_start, packet->consumer_events.yield_end,
        packet->consumer_events.resume_start,
        packet->consumer_events.resume_end,
        packet->consumer_events.notify_start,
        packet->consumer_events.notify_end);
#endif // PC_LOG
    event_buffer_push(events, (PerPacketEvents){
                                  .consumer = packet->consumer_events,
                                  .producer =
                                      (PerSideEvents){
                                          .yield_start = yield_start,
                                          .yield_end = yield_end,
                                          .resume_start = resume_start,
                                          .resume_end = resume_end,
                                          .work_start = work_start,
                                          .work_end = work_end,
                                          .notify_start = notify_start,
                                          .notify_end = notify_end,
                                      },
                              });
    spsc_queue_commit_pop(queue);

    if (spsc_queue_producer_needs_signal(queue)) {
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

#endif // !
