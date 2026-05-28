#!/bin/bash
# C910 IEW 修改前后对比测试脚本
# 用法: ./run_comparison.sh <before|after> <binary_path>

set -e

GEM5_ROOT="/home/mikasa/0508/gem5_modify"
cd "$GEM5_ROOT"
export PATH=/usr/bin:$PATH

MODE="$1"
BINARY="$2"

if [ -z "$MODE" ] || [ -z "$BINARY" ]; then
    echo "Usage: $0 <before|after> <binary_path>"
    exit 1
fi

# 编译对应版本
echo "=== 编译 $MODE 版本 ==="
scons build/RISCV/gem5.opt -j$(nproc) 2>&1 | tail -5

# 创建临时测试配置
TEST_DIR="/home/mikasa/0508/gem5_modify/tests"
OUTPUT_DIR="$TEST_DIR/comparison_results"
mkdir -p "$OUTPUT_DIR"

echo "=== 运行 $MODE 版本，测试程序: $BINARY ==="
build/RISCV/gem5.opt "$TEST_DIR/test_microbench_se.py" \
    --outdir="$OUTPUT_DIR/m5out_${MODE}" 2>&1 | tail -5

# 提取关键 stats
STATS_FILE="$OUTPUT_DIR/m5out_${MODE}/stats.txt"
RESULT_FILE="$OUTPUT_DIR/${MODE}_results.txt"

echo "=== 提取关键 stats ==="
{
    echo "# $MODE version results"
    echo "# Binary: $BINARY"
    echo ""
    grep -E "pipeIssueCount|stallCycles::|ex1Forward|ex2Writeback|branchMispredicts|fenceSyncCount|simTicks" "$STATS_FILE"
} > "$RESULT_FILE"

echo "$MODE 结果已保存到 $RESULT_FILE"
