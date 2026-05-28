#!/bin/bash
# Run C910 libquantum (SPEC 462.libquantum) test
# Usage: ./run_libquantum.sh [--build-only|--run-only|--stats-only]

set -e

GEM5_ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD="$GEM5_ROOT/build/RISCV/gem5.opt"
CONFIG="$GEM5_ROOT/configs/example/c910_libquantum_se.py"
STATS="$GEM5_ROOT/m5out/stats.txt"

cd "$GEM5_ROOT"

case "${1:-}" in
    --build-only)
        echo "=== Building gem5 ==="
        scons $BUILD -j$(nproc)
        exit 0
        ;;
    --run-only)
        ;;
    --stats-only)
        if [ ! -f "$STATS" ]; then
            echo "Error: $STATS not found. Run simulation first."
            exit 1
        fi
        python3 tests/c910/analyze_libquantum_stats.py "$STATS"
        exit 0
        ;;
esac

# ============================================================
# Step 1: Build (if needed)
# ============================================================
if [ ! -f "$BUILD" ]; then
    echo "=== Building gem5 ==="
    scons $BUILD -j$(nproc)
else
    echo "=== gem5 already built ==="
fi

# ============================================================
# Step 2: Run simulation
# ============================================================
echo ""
echo "=== Running libquantum (Shor's algorithm, N=15) ==="
$BUILD --outdir=m5out $CONFIG 2>&1 | tee m5out/simout.log

# ============================================================
# Step 3: Analyze stats
# ============================================================
echo ""
echo "=== Analyzing statistics ==="
if [ -f "$STATS" ]; then
    python3 tests/c910/analyze_libquantum_stats.py "$STATS"
else
    echo "Warning: stats.txt not found in m5out/"
    exit 1
fi
