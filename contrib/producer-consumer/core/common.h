#ifndef PC_CORE_COMMON_H
#define PC_CORE_COMMON_H

#include "portable.h"

typedef enum {
  CREND = 0,
  CRCLOSED = 1,
  CRINTERRUPTED = 2,
} CompResult;

typedef struct RuntimeConst RuntimeConst;
typedef struct RuntimeMut RuntimeMut;

/*
 * Represents the running context of the program.
 */
typedef struct {
  /*
   * Checks if the program received an interrupt request and must stop.
   */
  bool (*is_interrupted)(RuntimeConst self);

  /*
   * Fixes the calling software thread to its current hardware thread.
   * If it fails, it should abort the program.
   */
  void (*fix_hart)(RuntimeMut self);
} RuntimeOps;

declare_trait(Runtime, RuntimeOps);

#endif // !PC_CORE_COMMON_H
