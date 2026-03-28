# bench/metric_push/Makefile — Lightweight metric shipper (host x86, DOCA 3.1)

DOCA_DIR ?= /opt/mellanox/doca

CC      ?= gcc
CFLAGS   = -Wall -Wextra -O2 \
           -I$(DOCA_DIR)/include \
           -I../../common \
           -I../../tunnel

LDFLAGS  = -L$(DOCA_DIR)/lib/x86_64-linux-gnu \
           -ldoca_comch -ldoca_common -ldoca_argp \
           -Wl,-rpath,$(DOCA_DIR)/lib/x86_64-linux-gnu \
           -lpthread

TUNNEL_OBJ = ../../tunnel/host/comch_host.o

.PHONY: all clean

all: metric_push

$(TUNNEL_OBJ):
	$(MAKE) -C ../../tunnel/host COMCH_HOST_DOCA_VER=31

metric_push: metric_push.c $(TUNNEL_OBJ)
	$(CC) $(CFLAGS) -o $@ $< $(TUNNEL_OBJ) $(LDFLAGS)

clean:
	rm -f metric_push
