CFLAGS  = -Wall -O2 -I../../common
LDFLAGS = -lopenblas -lm -lpthread

all: gemm_bench

gemm_bench: gemm_bench.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -f gemm_bench
