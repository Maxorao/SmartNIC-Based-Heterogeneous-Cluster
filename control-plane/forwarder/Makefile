DOCA_DIR  ?= /opt/mellanox/doca
CFLAGS    = -Wall -Wextra -O2 \
            -I$(DOCA_DIR)/include \
            -I../../common \
            -I../../tunnel \
            -DCOMCH_NIC_DOCA_VER=15
LDFLAGS   = -L$(DOCA_DIR)/lib/aarch64-linux-gnu \
            -ldoca_comm_channel -ldoca_common -ldoca_argp \
            -Wl,-rpath,$(DOCA_DIR)/lib/aarch64-linux-gnu \
            -lpthread

CC ?= gcc

.PHONY: all clean

all: forward_routine

forward_routine: forward_routine.c ../../tunnel/nic/comch_nic.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -f forward_routine *.o
