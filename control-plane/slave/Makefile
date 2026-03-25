DOCA_DIR  ?= /opt/mellanox/doca
CFLAGS    = -Wall -Wextra -O2 \
            -I$(DOCA_DIR)/include \
            -I../../common \
            -I../../tunnel/host
LDFLAGS   = -L$(DOCA_DIR)/lib/x86_64-linux-gnu \
            -ldoca_comm_channel -ldoca_common -ldoca_argp \
            -Wl,-rpath,$(DOCA_DIR)/lib/x86_64-linux-gnu \
            -lpthread -lm

CC ?= gcc

.PHONY: all clean nodoca

all: slave_monitor

# Full build with DOCA offload support
slave_monitor: slave_monitor.c ../../tunnel/host/comch_host.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Direct-TCP-only build (no DOCA required, for baseline experiments)
nodoca: slave_monitor.c
	$(CC) -Wall -O2 -DNO_DOCA \
	      -I../../common \
	      -o slave_monitor_direct $< -lpthread -lm

clean:
	rm -f slave_monitor slave_monitor_direct *.o
