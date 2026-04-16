obj-m += monitor.o

KDIR := /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

# Workload binaries are built static so they run inside Alpine rootfs.
# Override with WORKLOAD_LDFLAGS= (empty) for CI / host-only builds.
WORKLOAD_LDFLAGS ?= -static

USER_TARGETS := engine memory_hog cpu_hog io_pulse

# ── Default: build everything ────────────────────────────────
all: $(USER_TARGETS) module

# ── CI-safe: user-space only, no kernel module ───────────────
# Used by GitHub Actions smoke check:  make -C boilerplate ci
ci: WORKLOAD_LDFLAGS =
ci: $(USER_TARGETS)

# ── Kernel module ─────────────────────────────────────────────
module: monitor.ko

monitor.ko: monitor.c monitor_ioctl.h
	$(MAKE) -C $(KDIR) M=$(PWD) modules

# ── User-space binaries ───────────────────────────────────────
engine: engine.c monitor_ioctl.h
	gcc -O2 -Wall -Wextra -o engine engine.c -lpthread

memory_hog: memory_hog.c
	gcc -O2 -Wall $(WORKLOAD_LDFLAGS) -o memory_hog memory_hog.c

cpu_hog: cpu_hog.c
	gcc -O2 -Wall $(WORKLOAD_LDFLAGS) -o cpu_hog cpu_hog.c

io_pulse: io_pulse.c
	gcc -O2 -Wall $(WORKLOAD_LDFLAGS) -o io_pulse io_pulse.c

# ── Clean ─────────────────────────────────────────────────────
clean:
	if [ -d "$(KDIR)" ]; then $(MAKE) -C $(KDIR) M=$(PWD) clean; fi
	rm -f $(USER_TARGETS)
	rm -f *.o *.mod *.mod.c *.symvers *.order
	rm -f *.log
	rm -rf logs
	rm -f /tmp/jackfruit.sock
	rm -f /tmp/jackfruit_*.log

.PHONY: all ci module clean
