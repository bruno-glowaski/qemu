#ifndef PC_CORE_TRACING_H
#define PC_CORE_TRACING_H

#include "portable.h"

typedef struct {
  tsc_t wait_start;
  tsc_t wait_end;
  tsc_t work_start;
  tsc_t work_end;
  tsc_t signal_start;
  tsc_t signal_end;
} PerSideEvents;

typedef struct {
  PerSideEvents producer;
  PerSideEvents consumer;
} PerPacketEvents;

#endif // !PC_CORE_TRACING_H
