CFLAGS ?= -O2 -Wall
LDLIBS = -lz

bench_native: bench_native.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f bench_native

.PHONY: clean
