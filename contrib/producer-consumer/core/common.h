#ifndef PC_CORE_COMMON_H
#define PC_CORE_COMMON_H

#include "portable.h"

typedef enum {
  CREND = 0,
  CRCLOSED = 1,
  CRINTERRUPTED = 2,
} CompResult;

/*
 * Represents the running context of the program.
 */
typedef struct Runtime Runtime;

typedef struct {
  /*
   * Checks if the program received an interrupt request and must stop.
   */
  bool (*is_interrupted)(const Runtime *self);

  /*
   * Fixes the calling software thread to its current hardware thread.
   * If it fails, it should abort the program.
   */
  void (*fix_hart)(Runtime *self);
} RuntimeOps;

typedef struct Runtime {
  const RuntimeOps *ops;
  void *data;
} Runtime;

#endif // !PC_CORE_COMMON_H
