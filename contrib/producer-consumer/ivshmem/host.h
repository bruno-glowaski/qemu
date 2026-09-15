#ifndef PC_IVSHMEM_HOST_H
#define PC_IVSHMEM_HOST_H

#include "contrib/producer-consumer/core/portable.h"
#include "contrib/producer-consumer/core/transport.h"

typedef struct IvshmemHostCreateInfo {
  bool is_primary;
  const char *server_path;
} IvshmemHostCreateInfo;

///
int ivshmem_host_create(const IvshmemHostCreateInfo *info, Transport *output);

#endif // PC_IVSHMEM_SERVER
