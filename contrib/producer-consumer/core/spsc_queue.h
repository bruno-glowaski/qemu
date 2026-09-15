#ifndef PC_CORE_SPSC_QUEUE_H
#define PC_CORE_SPSC_QUEUE_H

#include "portable.h"
#include "measurement.h"

/// A work packet
typedef struct {
  PerSideEvents consumer_events;
  unsigned char _padding[16];
} Packet;

/// A low-latency, lock-free shared queue oriented for single producer and
/// single consumer cases.
typedef struct {
  // Producer-owned state.
  cache_aligned atomic_uint64_t head;

  // Consumer-owned state.
  cache_aligned atomic_uint64_t tail;

  cache_aligned atomic_bool_t consumer_needs_signal;
  cache_aligned atomic_bool_t producer_needs_signal;

  /*
   * Immutable queue configuration.
   */

  // Total size of the queue.
  uint64_t capacity;
  cache_aligned Packet buffer[];
} SPSCQueue;

static inline uint64_t calculate_spsc_queue_capacity_for_size(uint64_t size) {
  if (size < offsetof(SPSCQueue, buffer))
    return 0;

  return (size - offsetof(SPSCQueue, buffer)) / sizeof(Packet);
}

static inline void spsc_queue_init(SPSCQueue *queue, uint64_t shm_size) {
  queue->capacity = calculate_spsc_queue_capacity_for_size(shm_size);
  au64_store_release(&queue->head, 0);
  au64_store_release(&queue->tail, 0);
}

/// Returns the next packet slot to pop. If queue is empty, returns `NULL`.
static inline Packet *spsc_queue_peek_pop(SPSCQueue *queue) {
  uint64_t tail = au64_load_relaxed(&queue->tail);
  uint64_t head = au64_load_acquire(&queue->head);

  if (tail == head)
    return NULL;

  return &queue->buffer[tail % queue->capacity];
}

/// Frees the packet slot to be used by the consumer.
static inline void spsc_queue_commit_pop(SPSCQueue *queue) {
  uint64_t tail = au64_load_relaxed(&queue->tail);
  au64_store_release(&queue->tail, tail + 1);
}

/// Returns the next packet slot to push. If queue is empty, returns `NULL`.
static inline Packet *spsc_queue_peek_push(SPSCQueue *queue) {
  uint64_t head = au64_load_relaxed(&queue->head);
  uint64_t tail = au64_load_acquire(&queue->tail);

  if (head - tail == queue->capacity)
    return NULL;

  return &queue->buffer[head % queue->capacity];
}

/// Confirms the packet slot as pushed.
static inline void spsc_queue_commit_push(SPSCQueue *queue) {
  uint64_t head = au64_load_relaxed(&queue->head);
  au64_store_release(&queue->head, head + 1);
}

/// Checks if the producer needs a signal.
static inline bool spsc_queue_producer_needs_signal(SPSCQueue *queue) {
  return ab_load_acquire(&queue->producer_needs_signal);
}

/// Checks if the consumer needs a signal.
static inline bool spsc_queue_consumer_needs_signal(SPSCQueue *queue) {
  return ab_load_acquire(&queue->consumer_needs_signal);
}

/// Requests a signal for producer.
static inline void spsc_queue_request_signal_for_producer(SPSCQueue *queue) {
  ab_store_release(&queue->producer_needs_signal, true);
}

/// Requests a signal for consumer.
static inline void spsc_queue_request_signal_for_consumer(SPSCQueue *queue) {
  ab_store_release(&queue->consumer_needs_signal, true);
}

/// Clears signal requests for producer.
static inline void spsc_queue_clear_signal_for_producer(SPSCQueue *queue) {
  ab_store_release(&queue->producer_needs_signal, false);
}

/// Clears signal requests for consumer.
static inline void spsc_queue_clear_signal_for_consumer(SPSCQueue *queue) {
  ab_store_release(&queue->consumer_needs_signal, false);
}

#endif // PC_CORE_TRANSPORT_H
