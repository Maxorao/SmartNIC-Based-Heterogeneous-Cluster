CFLAGS  = -Wall -Wextra -O2 -I../../common
LDFLAGS = -lpq -lpthread

CC ?= gcc

.PHONY: all clean

all: master_monitor

master_monitor: master_monitor.c db.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -f master_monitor *.o
