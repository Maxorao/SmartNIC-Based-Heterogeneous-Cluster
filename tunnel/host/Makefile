DOCA_DIR  ?= /opt/mellanox/doca
CFLAGS    = -Wall -Wextra -O2 \
            -I$(DOCA_DIR)/include \
            -I../../common
LDFLAGS   = -L$(DOCA_DIR)/lib/x86_64-linux-gnu \
            -ldoca_comm_channel -ldoca_common -ldoca_argp \
            -Wl,-rpath,$(DOCA_DIR)/lib/x86_64-linux-gnu

CC ?= gcc

.PHONY: all clean

all: comch_host.o

comch_host.o: comch_host.c comch_host.h ../../common/protocol.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f *.o
