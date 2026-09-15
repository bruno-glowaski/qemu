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
  tsc_t work_cost;

  Runtime *runtime;
  Transport *transport;
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
  events->data[events->capacity++] = data;
}

static inline int run_consumer(const ConsumerInfo *info) {
  int res = 0;
  SPSCQueue *queue;
  Packet *packet;
  tsc_t work_cost, wait_start, wait_end, work_start, work_end, signal_start = 0,
                                                               signal_end = 0;
  Runtime *runtime = info->runtime;
  Transport *transport = info->transport;
  EventBuffer *events = info->events;

  work_cost = info->work_cost;
  queue = transport->ops->get_queue(transport);

  runtime->ops->fix_hart(runtime);

  for (;;) {
    if (event_buffer_is_filled(events)) {
      res = CREND;
      goto end;
    }
    if (runtime->ops->is_interrupted(runtime)) {
      res = CRINTERRUPTED;
      goto end;
    }
    if (transport->ops->is_closed(transport)) {
      res = CRCLOSED;
      goto end;
    }

    packet = spsc_queue_peek_push(queue);
    if (packet == NULL) {
      do {
        wait_start = read_tsc();
        res = transport->ops->wait_until(transport);
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

    event_buffer_push(events, (PerPacketEvents){
                                  .consumer = packet->consumer_events,
                                  .producer =
                                      (PerSideEvents){
                                          .wait_start = wait_start,
                                          .wait_end = wait_end,
                                          .work_start = work_start,
                                          .work_end = work_end,
                                          .signal_start = signal_start,
                                          .signal_end = signal_end,
                                      },
                              });

    spsc_queue_commit_push(queue);

    if (spsc_queue_consumer_needs_signal(queue)) {
      signal_start = read_tsc();
      res = transport->ops->notify(transport);
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
