DOCA_DIR  ?= /opt/mellanox/doca
CFLAGS    = -Wall -Wextra -O2 \
            -I$(DOCA_DIR)/include \
            -I../../common \
            -I../../tunnel/nic
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
