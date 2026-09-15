#ifndef PC_IVSHMEM_GUEST_H
#define PC_IVSHMEM_GUEST_H

#include "contrib/producer-consumer/core/transport.h"

typedef struct IvshmemGuestCreateInfo {
} IvshmemGuestCreateInfo;

int ivshmem_guest_create(const IvshmemGuestCreateInfo *info, Transport *output);

#endif // PC_IVSHMEM_SERVER
