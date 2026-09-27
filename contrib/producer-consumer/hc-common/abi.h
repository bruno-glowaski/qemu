#ifndef PC_HC_COMMON_ABI_H
#define PC_HC_COMMON_ABI_H

#define HCC_VENDOR_ID 0x1b36
#define HCC_DEVICE_ID 0x0001

#define HC_BAR_REGS 0
#define HC_BAR_MSIX 1
#define HC_BAR_SHARED 2

#define HC_BAR_SIZE_REGS 0x1000
#define HC_BAR_SIZE_MSIX 0x1000

// RO 32-bit
#define HC_REG_ABI_VERSION 0x000
// RO 32-bit
#define HC_REG_MODE 0x008
// WO 32-bit
#define HC_REG_PROD_STATUS 0x010
// RO 32-bit
#define HC_REG_CONS_STATUS 0x014
// RO 64-bit
#define HC_REG_SHARED_SIZE 0x018
// WO 32-bit
#define HC_REG_DOORBELL 0x020

#define HC_MSIX_TABLE_OFFSET 0x0000
#define HC_MSIX_PBA_OFFSET 0x0800

enum hc_abi_version { HC_ABI_VERSION_1_0 = 0 };
enum hc_mode { HC_MODE_POLL = 0, HC_MODE_SLEEP = 1, HC_MODE_NOTIFY = 2 };
enum hc_side_status {
  HC_SIDE_STATUS_PENDING = 0,
  HC_SIDE_STATUS_READY = 1,
  HC_SIDE_STATUS_CLOSED = 2
};

#endif // !PC_HC_COMMON_ABI_H
