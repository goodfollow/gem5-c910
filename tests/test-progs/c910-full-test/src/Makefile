CROSS = riscv64-unknown-linux-gnu-
CC = $(CROSS)gcc
CFLAGS = -O2 -static -march=rv64gc -Wall -Wextra

OUTDIR = ../bin/riscv/linux

all: $(OUTDIR)/c910_iew_full_test

$(OUTDIR)/c910_iew_full_test: c910_iew_full_test.c
	@mkdir -p $(OUTDIR)
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(OUTDIR)/c910_iew_full_test

.PHONY: all clean
