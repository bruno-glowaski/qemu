import fcntl
import struct

HCP_IOCTL_RUN = (
    (1 << 30)             # _IOC_WRITE
    | (ord("H") << 8)     # _IOC_TYPE
    | (0 << 0)            # _IOC_NR
    | (32 << 16)          # _IOC_SIZE
)

config = struct.pack("=QQQQ", 3000, 3000, 3000, 3000)

assert struct.calcsize("=QQQQ") == 32

with open("/dev/hc-producer", "wb", buffering=0) as dev:
    fcntl.ioctl(dev.fileno(), HCP_IOCTL_RUN, config)
