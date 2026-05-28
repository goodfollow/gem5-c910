# C910 IEW Full Test Suite

A comprehensive test suite for verifying the correctness of OpenC910-style IEW stage modifications in gem5 O3 CPU.

## Overview

The test suite consists of four components:

| File | Purpose |
|------|---------|
| `tests/test-progs/c910-full-test/src/c910_iew_full_test.c` | RISC-V binary (~800 lines) with 20+ test sections |
| `configs/example/c910_full_test_se.py` | gem5 SE config with C910-like O3CPU parameters |
| `tests/c910/analyze_full_test_stats.py` | Python script to validate gem5 stats.txt |
| `tests/c910/run_full_test.sh` | Shell script to build + run + analyze |

## Quick Start

```bash
# From gem5 root directory:
./tests/c910/run_full_test.sh
```

Or step by step:

```bash
# 1. Build the test binary
cd tests/test-progs/c910-full-test/src
riscv64-unknown-linux-gnu-gcc -O2 -static -march=rv64gc \
    -o ../bin/riscv/linux/c910_iew_full_test c910_iew_full_test.c

# 2. Run simulation
cd ../../../..  # back to gem5 root
build/RISCV/gem5.opt configs/example/c910_full_test_se.py

# 3. Analyze stats
python3 tests/c910/analyze_full_test_stats.py m5out/stats.txt
```

## Test Coverage Matrix

### 1. IQ Type Routing (7 tests)
| Test | Target IQ | Pipe | OpClass |
|------|-----------|------|---------|
| AIQ0 routing | AIQ0 | Pipe0/Pipe1 | IntAlu (ADD/SUB/AND/OR/XOR/Shift/SLT) |
| AIQ1 routing | AIQ1 | Pipe1 | IntMult (MUL/MADD) |
| BIQ routing | BIQ | Pipe2 | Branch/Jump (unpredictable pattern) |
| LSIQ routing | LSIQ | Pipe3 | Load (sequential + random) |
| SDIQ routing | SDIQ | Pipe5 | Store (dense pattern + verify) |
| VMB routing | VMB | - | Scalar fallback (no RVV in SE) |
| VIQ0 routing | VIQ1 | Pipe6 | Scalar FP (FloatAdd/Div) |

### 2. IQ Overflow & Fallback (2 tests)
| Test | Trigger | Fallback Path |
|------|---------|---------------|
| BIQ overflow (12 entries) | 500 branches | BIQ → AIQ0 |
| AIQ1 overflow (11 entries) | 300 MULs | AIQ1 → AIQ0 |

### 3. Stall Types (3 tests)
| Test | Expected Stall | Verification |
|------|----------------|--------------|
| DISPATCH stall | DISPATCH_STALL, IQ_FULL | Bandwidth saturation with mixed ops |
| DIV stall | DIV_STALL | 50 back-to-back divisions (20 cycles each) |
| DIV+ALU mixed | DIV_STALL + NO_STALL | Interleaved slow/fast ops |

### 4. EX1 vs EX2 Path (3 tests)
| Test | Path | Expected |
|------|------|----------|
| EX1 only | EX1 (1 cycle, comb forwarding) | ex1ForwardInsts >> 0 |
| EX2 only | EX2 (3-5 cycles, clock-edge WB) | ex2WritebackInsts >> 0 |
| Mixed EX1+EX2 | Both | Dependency chain across latencies |

### 5. FENCE Ordering (1 test)
| Test | Verification |
|------|--------------|
| Interleaved FENCE | Memory ordering with store → FENCE → load |

### 6. Store-Load Forwarding (3 tests)
| Test | Pattern | Expected |
|------|---------|----------|
| Immediate | Store then immediately load | 0 errors |
| Strided | Store/load at stride 4 | 0 errors |
| Randomized | Random index, immediate load | 0 errors |

### 7. Multi-PRF Scoreboard (2 tests)
| Test | PRF Type | Verification |
|------|----------|--------------|
| PREG stress | Integer (95 phys regs) | 200-entry dependency chain |
| EREG stress | Scalar FP (48 phys regs) | 100-entry FP chain with ADD/MUL/DIV |

### 8. Writeback Port Classification (2 tests)
| Test | Port | Verification |
|------|------|--------------|
| PREG WB | pregWBWidth=3 | 5000 integer ops burst |
| EREG WB | eregWBWidth=2 | 3000 FP ops burst |

### 9. Mixed Stress (1 test)
50000 iterations with per-iteration: 3x ALU + 1x MUL + 1x DIV/50 + 1x Branch + 1x Load/10 + 1x Store/10 + 2x FP + 1x FP_MUL/20

### 10. Dependency Chains (3 tests)
| Test | Pattern | Depth |
|------|---------|-------|
| Fan-out | 1 producer → 8 consumers | 1→8 |
| Fan-in | 8 producers → 1 consumer | 8→1 |
| Long chain | Sequential dependency | 10000 deep |

### 11. Edge Cases (3 tests)
| Test | What |
|------|------|
| Zero/negative | Correct arithmetic with 0 and negative values |
| Large immediates | MAX_INT64 / MIN_INT64 handling |
| FP comparison | 0==-0, NaN!=NaN, Inf comparisons |

## Expected Statistics

After a successful run, the stats.txt should show:

| Statistic | Expected |
|-----------|----------|
| `pipeIssueCount::pipe0_iu_alu` | > 50000 |
| `pipeIssueCount::pipe1_iu_alu_mla` | > 500 |
| `pipeIssueCount::pipe2_bju` | > 50000 |
| `pipeIssueCount::pipe3_lsu_load` | > 5000 |
| `pipeIssueCount::pipe4_lsu_special` | > 25 |
| `pipeIssueCount::pipe5_lsu_store` | > 5000 |
| `pipeIssueCount::pipe6_vfpu_alu` | > 3000 |
| `pipeIssueCount::pipe7_vfpu_ma` | > 100 |
| `stallCycles::div_stall` | > 0 |
| `ex1ForwardInsts` | > ex2WritebackInsts |
| `ex2WritebackInsts` | > 0 |
| `ex2LatencyHist` | > 0 |
| `totalWakeDependents` | > 0 |
| `fenceSyncCount` | > 0 |
| `committedInsts` | > 10000 |
| `iqEmptyCycles` | > 0 |
| `pipelineEmptyCycles` | > 0 |
| `pipelineStallCycles` | > 0 |

## Debugging

### Enable detailed IEW logging
```bash
build/RISCV/gem5.opt --debug-flags=IQ,IEW,Scoreboard,Exec configs/example/c910_full_test_se.py
```

### View debug log
```bash
less m5out/c910_full_test.se-<timestamp>/simout
```

### Compare with baseline
```bash
# Run with baseline (non-modified) gem5
build/RISCV/gem5.opt configs/example/c910_full_test_se.py --outdir=m5out_baseline
# Run with modified gem5
build/RISCV/gem5.opt configs/example/c910_full_test_se.py --outdir=m5out_modified
# Compare
diff m5out_baseline/stats.txt m5out_modified/stats.txt
```
