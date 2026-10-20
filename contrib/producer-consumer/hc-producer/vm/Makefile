SHELL := /bin/bash
include versions.env
export

BASE := ubuntu-base.img
DISK := ubuntu-dev.qcow2
SEED := seed.img

QEMU := ../../../../build/qemu-system-x86_64

.PHONY: all download seed run clean

all: download seed

download:
	test -f "$(BASE)" || \
	  curl -fL "$(UBUNTU_IMAGE_URL)" -o "$(BASE)"

seed:
	cloud-localds "$(SEED)" user-data meta-data

run: all
	@if [ ! -f "$(DISK)" ]; then \
	  qemu-img create -f qcow2 -F qcow2 \
	    -b "$(BASE)" "$(DISK)" 30G; \
	fi
	$(QEMU) \
	  -machine accel=kvm:tcg -enable-kvm -cpu host -m 2048 -smp 2 \
          -device hc-consumer \
          -rtc base=localtime,clock=host \
	  -net user -net nic \
	  -global isa-fdc.fdtypeA=none \
	  -drive file="$(DISK)",index=0,if=virtio,format=qcow2,media=disk \
	  -drive file="$(SEED)",index=1,if=virtio,format=raw,media=cdrom

clean:
	rm -f "$(DISK)" "$(SEED)"
