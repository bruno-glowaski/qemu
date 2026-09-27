#ifndef PC_CORE_TRANSPORT_H
#define PC_CORE_TRANSPORT_H

#include "portable.h"
#include "spsc_queue.h"

typedef struct TransportConst TransportConst;
typedef struct TransportMut TransportMut;

/*
 * Represents an endpoint for obtaining a shared queue, sending and receiving
 * notifications.
 */
typedef struct TransportOps {
  /*
   * Returns the memory address of the shared queue.
   */
  SPSCQueue *(*get_queue)(TransportConst self);

  /*
   * Blocks until the other side is ready to work. Returns 0 on success,
   * negative on error, positive on completion.
   */
  int (*ready_barrier_wait)(TransportMut self);

  /*
   * Sends a notification to the other side of the link.
   */
  int (*notify)(TransportMut self);

  /*
   * Waits until a notification is signaled from the other side. Returns 0 on
   * success, negative on error, positive on completion.
   */
  int (*wait_until)(TransportMut self);

  /*
   * Checks if the transport has been broken.
   */
  bool (*is_closed)(TransportConst self);

  /*
   * Closes the communication from this side.
   */
  void (*close)(TransportMut self);
} TransportOps;

declare_trait(Transport, TransportOps);

#endif // PC_CORE_TRANSPORT_H
