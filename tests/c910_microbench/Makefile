# Makefile for C910 IEW Micro-Benchmarks
#
# Usage:
#   make              # Build all micro-benchmarks
#   make run          # Build and run in gem5 (requires GEM5_CONFIG)
#   make clean        # Remove built files

CC      = riscv64-linux-gnu-gcc
CFLAGS  = -O0 -static -Wall -Wextra -march=rv64imafdc -mabi=lp64d
LDFLAGS = -static

SRCS    = c910_microbench_runner.c
TARGET  = c910_microbench

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $(TARGET) $<
	@echo "Built $(TARGET) (RISC-V ELF)"
	@file $(TARGET)

clean:
	rm -f $(TARGET)

run: $(TARGET)
	@echo "Running $(TARGET) in gem5..."
	@echo "Usage: gem5.opt <your-config> -- $(TARGET)"
