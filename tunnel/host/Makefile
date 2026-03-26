# tunnel/host/Makefile — host-side Comch (x86, DOCA 3.1 by default)
#
# COMCH_HOST_DOCA_VER selects implementation:
#   31 = DOCA 3.x  (default — current lab: tianjin/fujian/helong)
# Override: make COMCH_HOST_DOCA_VER=31

DOCA_DIR             ?= /opt/mellanox/doca
COMCH_HOST_DOCA_VER  ?= 31

CC     ?= gcc
CFLAGS  = -Wall -Wextra -O2 \
          -I$(DOCA_DIR)/include \
          -I../../common \
          -I.. \
          -DCOMCH_HOST_DOCA_VER=$(COMCH_HOST_DOCA_VER)

# DOCA 3.1 uses libdoca_comch (not libdoca_comm_channel)
LDFLAGS = -L$(DOCA_DIR)/lib/x86_64-linux-gnu \
          -ldoca_comch -ldoca_common -ldoca_argp \
          -Wl,-rpath,$(DOCA_DIR)/lib/x86_64-linux-gnu \
          -lpthread

.PHONY: all clean info

all: comch_host.o

# comch_host.c #include's the selected impl file — list both as deps
comch_host.o: comch_host.c comch_host_doca31.c ../comch_api.h
	$(CC) $(CFLAGS) -c -o $@ comch_host.c

info:
	@echo "COMCH_HOST_DOCA_VER = $(COMCH_HOST_DOCA_VER)"
	@ls $(DOCA_DIR)/lib/x86_64-linux-gnu/libdoca_comch* 2>/dev/null \
	  || echo "WARNING: libdoca_comch not found"

clean:
	rm -f *.o
