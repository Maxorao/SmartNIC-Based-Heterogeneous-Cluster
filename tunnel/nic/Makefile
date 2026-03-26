# tunnel/nic/Makefile — NIC-side Comch (BF ARM)
#
# COMCH_NIC_DOCA_VER selects implementation:
#   15 = DOCA 1.5  (default — current lab: BF2, DOCA 1.5.4003)
#   31 = DOCA 3.x  (future — BF3 migration target)
# Override: make COMCH_NIC_DOCA_VER=31

DOCA_DIR            ?= /opt/mellanox/doca
COMCH_NIC_DOCA_VER  ?= 15

CC     ?= gcc
CFLAGS  = -Wall -Wextra -O2 \
          -I$(DOCA_DIR)/include \
          -I../../common \
          -I.. \
          -DCOMCH_NIC_DOCA_VER=$(COMCH_NIC_DOCA_VER)

# DOCA 1.5: libdoca_comm_channel  |  DOCA 3.x: libdoca_comch
ifeq ($(shell test $(COMCH_NIC_DOCA_VER) -ge 30 && echo yes),yes)
DOCA_COMCH_LIB = -ldoca_comch
else
DOCA_COMCH_LIB = -ldoca_comm_channel
endif

LDFLAGS = -L$(DOCA_DIR)/lib/aarch64-linux-gnu \
          $(DOCA_COMCH_LIB) -ldoca_common -ldoca_argp \
          -Wl,-rpath,$(DOCA_DIR)/lib/aarch64-linux-gnu \
          -lpthread

.PHONY: all clean info

all: comch_nic.o

# comch_nic.c #include's the selected impl file — list all as deps
comch_nic.o: comch_nic.c comch_nic_doca15.c comch_nic_doca31.c ../comch_api.h
	$(CC) $(CFLAGS) -c -o $@ comch_nic.c

info:
	@echo "COMCH_NIC_DOCA_VER  = $(COMCH_NIC_DOCA_VER)"
	@echo "DOCA_COMCH_LIB      = $(DOCA_COMCH_LIB)"
	@ls $(DOCA_DIR)/lib/aarch64-linux-gnu/libdoca_comm_channel* 2>/dev/null \
	  || echo "WARNING: libdoca_comm_channel not found"

clean:
	rm -f *.o
