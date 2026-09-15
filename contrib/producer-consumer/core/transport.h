#ifndef PC_CORE_TRANSPORT_H
#define PC_CORE_TRANSPORT_H

#include "spsc_queue.h"

typedef struct Transport Transport;
typedef struct TransportOps {
  /// Returns the memory address of the shared queue.
  SPSCQueue *(*get_queue)(Transport self);
  /// Sends a notification to the other side of the link.
  int (*notify)(Transport self);
  /// Waits until a notification is signaled from the other side.
  int (*wait_until)(Transport self);
  /// Checks if the transport has been broken.
  int (*is_closed)(Transport self);
  /// Closes the connection and destroys this link.
  void (*destroy)(Transport self);
} TransportOps;

/// Represents an endpoint for obtaining a shared queue, sending and receiving
/// notifications.
typedef struct Transport {
  const TransportOps *ops;
  void *data;
} Transport;

#endif // PC_CORE_TRANSPORT_H
