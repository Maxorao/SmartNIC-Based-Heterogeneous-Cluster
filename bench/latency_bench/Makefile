DOCA_DIR  ?= /opt/mellanox/doca

HOST_CFLAGS  = -Wall -Wextra -O2 \
               -I$(DOCA_DIR)/include \
               -I../../common \
               -I../../tunnel/host
HOST_LDFLAGS = -L$(DOCA_DIR)/lib/x86_64-linux-gnu \
               -ldoca_comm_channel -ldoca_common -ldoca_argp \
               -Wl,-rpath,$(DOCA_DIR)/lib/x86_64-linux-gnu

NIC_CFLAGS   = -Wall -Wextra -O2 \
               -I$(DOCA_DIR)/include \
               -I../../common \
               -I../../tunnel/nic
NIC_LDFLAGS  = -L$(DOCA_DIR)/lib/aarch64-linux-gnu \
               -ldoca_comm_channel -ldoca_common -ldoca_argp \
               -Wl,-rpath,$(DOCA_DIR)/lib/aarch64-linux-gnu \
               -lpthread

CC ?= gcc

.PHONY: all host nic clean

# Default: build bench_host (runs on x86)
all: bench_host

host: bench_host

bench_host: bench_host.c ../../tunnel/host/comch_host.c
	$(CC) $(HOST_CFLAGS) -o $@ $^ $(HOST_LDFLAGS)

# Build bench_nic on ARM (or cross-compile with CROSS_COMPILE prefix)
nic: bench_nic

bench_nic: bench_nic.c ../../tunnel/nic/comch_nic.c
	$(CROSS_COMPILE)$(CC) $(NIC_CFLAGS) -o $@ $^ $(NIC_LDFLAGS)

clean:
	rm -f bench_host bench_nic *.o
