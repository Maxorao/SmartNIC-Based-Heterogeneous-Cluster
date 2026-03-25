DOCA_DIR  ?= /opt/mellanox/doca
CFLAGS    = -Wall -Wextra -O2 \
            -I$(DOCA_DIR)/include \
            -I../../common
LDFLAGS   = -L$(DOCA_DIR)/lib/aarch64-linux-gnu \
            -ldoca_comm_channel -ldoca_common -ldoca_argp \
            -Wl,-rpath,$(DOCA_DIR)/lib/aarch64-linux-gnu

CC ?= gcc

.PHONY: all clean

all: comch_nic.o

comch_nic.o: comch_nic.c comch_nic.h ../../common/protocol.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f *.o
