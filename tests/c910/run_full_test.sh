#!/bin/bash
# run_full_test.sh — Build and run the C910 IEW full test suite
#
# Usage:
#   ./run_full_test.sh              # Build + run + analyze
#   ./run_full_test.sh --build-only # Only build binary
#   ./run_full_test.sh --run-only   # Only run simulation (no build)
#   ./run_full_test.sh --stats-only # Only analyze stats (no build/run)
#
# Prerequisites:
#   - riscv64-unknown-linux-gnu-gcc in PATH
#   - build/RISCV/gem5.opt compiled
#   - Run from gem5 root directory

set -e

GEM5_ROOT="$(pwd)"
TEST_SRC="tests/test-progs/c910-full-test/src"
TEST_BIN="tests/test-progs/c910-full-test/bin/riscv/linux/c910_iew_full_test"
CONFIG="configs/example/c910_full_test_se.py"
STATS_FILE="m5out/stats.txt"
ANALYZER="tests/c910/analyze_full_test_stats.py"
M5OUT="m5out"

build() {
    echo "============================================"
    echo " Building c910_iew_full_test..."
    echo "============================================"

    if ! command -v riscv64-unknown-linux-gnu-gcc &>/dev/null; then
        if ! command -v riscv64-linux-gnu-gcc &>/dev/null; then
            echo "ERROR: No RISC-V cross compiler found."
            echo "  Install: sudo apt install gcc-riscv64-linux-gnu"
            echo "  Or set up riscv64-unknown-linux-gnu-gcc in PATH"
            exit 1
        fi
        CROSS="riscv64-linux-gnu-"
    else
        CROSS="riscv64-unknown-linux-gnu-"
    fi

    mkdir -p "$(dirname "$TEST_BIN")"
    ${CROSS}gcc -O2 -static -march=rv64gc -Wall -Wextra \
        -o "$TEST_BIN" "$TEST_SRC/c910_iew_full_test.c"

    echo "Built: $TEST_BIN"
    echo "Size: $(du -h "$TEST_BIN" | cut -f1)"
    echo ""
}

run_sim() {
    echo "============================================"
    echo " Running simulation..."
    echo "============================================"

    if [ ! -f "build/RISCV/gem5.opt" ]; then
        echo "ERROR: build/RISCV/gem5.opt not found."
        echo "  Run: scons build/RISCV/gem5.opt -j$(nproc)"
        exit 1
    fi

    # Clean previous m5out
    rm -rf "$M5OUT"

    build/RISCV/gem5.opt "$CONFIG"

    if [ ! -f "$STATS_FILE" ]; then
        echo "ERROR: $STATS_FILE not generated."
        exit 1
    fi

    echo ""
    echo "Stats saved to: $STATS_FILE"
    echo ""
}

analyze() {
    echo "============================================"
    echo " Analyzing statistics..."
    echo "============================================"

    if [ ! -f "$STATS_FILE" ]; then
        echo "ERROR: $STATS_FILE not found. Run simulation first."
        exit 1
    fi

    python3 "$ANALYZER" "$STATS_FILE"
}

# ============================================================
# Main
# ============================================================
case "${1:-}" in
    --build-only)
        build
        ;;
    --run-only)
        run_sim
        ;;
    --stats-only)
        analyze
        ;;
    *)
        build
        run_sim
        analyze
        ;;
esac
