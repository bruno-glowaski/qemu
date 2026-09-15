#ifndef PC_IVSHMEM_COMMON_H
#define PC_IVSHMEM_COMMON_H

#include "contrib/producer-consumer/core/portable.h"

typedef struct IvshmemPcControl {
  atomic_bool is_ready;
} IvshmemPcControl;

#endif // !PC_IVSHMEM_COMMON_H
