#ifndef PC_CORE_TRACING_H
#define PC_CORE_TRACING_H

#include "portable.h"

typedef struct {
  tsc_t yield_start;
  tsc_t yield_end;
  tsc_t resume_start;
  tsc_t resume_end;
  tsc_t work_start;
  tsc_t work_end;

  /*
   * notify_start intentionally describe the notification
   * associated with the previous packet. The notification happens
   * after this event is recorded.
   */
  tsc_t notify_start;

  /*
   * notify_end intentionally describe the notification
   * associated with the previous packet. The notification happens
   * after this event is recorded.
   */
  tsc_t notify_end;
} PerSideEvents;

typedef struct {
  PerSideEvents producer;
  PerSideEvents consumer;
} PerPacketEvents;

#endif // !PC_CORE_TRACING_H
