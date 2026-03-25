CFLAGS  = -Wall -O2 -I../../common
LDFLAGS = -lpthread

all: mock_slave

mock_slave: mock_slave.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -f mock_slave
