#!/usr/bin/env python3
"""
analyze_full_test_stats.py — Validate gem5 statistics after C910 IEW full test

Usage:
    build/RISCV/gem5.opt configs/example/c910_full_test_se.py
    python3 analyze_full_test_stats.py m5out/stats.txt

This script parses the gem5 stats.txt and checks:
  1. All 8 pipe issue counters > 0 (Pipe0-Pipe7 exercised)
  2. Stall type distribution is non-trivial (stalls occurred)
  3. EX1 vs EX2 path separation (both paths used)
  4. IQ type read/write activity
  5. Scoreboard wake/ready counts
  6. FENCE synchronization events
  7. Total instruction count matches expectations
  8. Writeback port utilization

Exit code 0 if all checks pass, 1 if any fail.
"""

import os
import re
import sys


def parse_stats(filepath):
    """Parse gem5 stats.txt into a dict of {name: value}."""
    stats = {}
    with open(filepath) as f:
        for line in f:
            # Match lines like: system.cpu.iew.stallCycles::no_stall  123456
            m = re.match(r"^\s*([\w\.:_]+)\s+([0-9.eE+\-]+)", line)
            if m:
                name = m.group(1)
                try:
                    val = float(m.group(2))
                except ValueError:
                    continue
                stats[name] = val
    return stats


def get_val(stats, pattern, default=0.0):
    """Find the first stat matching pattern, return its value."""
    for name, val in stats.items():
        if re.search(pattern, name):
            return val
    return default


def check(name, condition, detail=""):
    """Print check result."""
    status = "PASS" if condition else "FAIL"
    msg = f"[{status}] {name}"
    if detail:
        msg += f"  ({detail})"
    print(msg)
    return condition


def main():
    if len(sys.argv) < 2:
        print("Usage: python3 analyze_full_test_stats.py m5out/stats.txt")
        sys.exit(1)

    filepath = sys.argv[1]
    if not os.path.exists(filepath):
        print(f"Error: File {filepath} not found")
        sys.exit(1)

    stats = parse_stats(filepath)
    print(f"Parsed {len(stats)} statistics from {filepath}\n")

    results = []

    # ============================================================
    # 1. Pipe Issue Counts — All 8 pipes exercised
    # ============================================================
    print("=== 1. Pipe Issue Counts ===")
    pipe_names = [
        "pipe0_iu_alu",
        "pipe1_iu_alu_mla",
        "pipe2_bju",
        "pipe3_lsu_load",
        "pipe4_lsu_special",
        "pipe5_lsu_store",
        "pipe6_vfpu_alu",
        "pipe7_vfpu_ma",
    ]

    for pipe_name in pipe_names:
        val = get_val(stats, pipe_name)
        ok = val > 0
        results.append(check(f"  {pipe_name}", ok, f"count={val:.0f}"))

    # ============================================================
    # 2. Stall Type Distribution
    # ============================================================
    print("\n=== 2. Stall Type Distribution ===")
    stall_names = [
        "no_stall",
        "rob_full",
        "iq_full",
        "vmb_full",
        "type_stall",
        "dispatch_stall",
        "is_stall",
        "div_stall",
        "vdiv_stall",
        "backend_stall",
    ]

    total_stall_cycles = 0
    has_stalls = False
    for stall_name in stall_names:
        val = get_val(stats, stall_name)
        if stall_name != "no_stall" and val > 0:
            has_stalls = True
        total_stall_cycles += val
        print(f"  {stall_name:20s} = {val:.0f} cycles")

    results.append(
        check(
            "  Total stall cycles",
            total_stall_cycles > 0,
            f"total={total_stall_cycles:.0f}",
        )
    )
    results.append(check("  At least one stall type active", has_stalls))

    # ============================================================
    # 3. EX1 vs EX2 Path Separation
    # ============================================================
    print("\n=== 3. EX1 vs EX2 Path ===")
    ex1_val = get_val(stats, "ex1ForwardInsts")
    ex2_val = get_val(stats, "ex2WritebackInsts")
    results.append(
        check("  EX1 forward insts", ex1_val > 0, f"count={ex1_val:.0f}")
    )
    results.append(
        check("  EX2 writeback insts", ex2_val > 0, f"count={ex2_val:.0f}")
    )
    results.append(
        check(
            "  EX1 > EX2 (more single-cycle ops)",
            ex1_val > ex2_val,
            f"EX1={ex1_val:.0f}, EX2={ex2_val:.0f}",
        )
    )

    # EX2 Latency Histogram
    ex2_hist = get_val(stats, "ex2LatencyHist")
    results.append(
        check(
            "  EX2 latency histogram sampled",
            ex2_hist > 0,
            f"total={ex2_hist:.0f}",
        )
    )

    # ============================================================
    # 4. IQ Type Read/Write Activity
    # ============================================================
    print("\n=== 4. IQ Type Activity ===")
    iq_reads = {
        "intInstQueueReads": get_val(stats, "intInstQueueReads"),
        "fpInstQueueReads": get_val(stats, "fpInstQueueReads"),
        "vecInstQueueReads": get_val(stats, "vecInstQueueReads"),
    }
    iq_writes = {
        "intInstQueueWrites": get_val(stats, "intInstQueueWrites"),
        "fpInstQueueWrites": get_val(stats, "fpInstQueueWrites"),
        "vecInstQueueWrites": get_val(stats, "vecInstQueueWrites"),
    }

    for name, val in iq_reads.items():
        print(f"  {name:30s} = {val:.0f}")
    for name, val in iq_writes.items():
        print(f"  {name:30s} = {val:.0f}")

    results.append(
        check(
            "  IQ reads occurred",
            any(v > 0 for v in iq_reads.values()),
            f"total={sum(iq_reads.values()):.0f}",
        )
    )
    results.append(
        check(
            "  IQ writes occurred",
            any(v > 0 for v in iq_writes.values()),
            f"total={sum(iq_writes.values()):.0f}",
        )
    )

    # ============================================================
    # 5. Scoreboard Wake/Ready Counts
    # ============================================================
    print("\n=== 5. Scoreboard Activity ===")
    wake_count = get_val(stats, "totalWakeDependents")
    insts_added = get_val(stats, "instsAdded")
    insts_executed = get_val(stats, "executeStats0.numInsts")

    results.append(
        check(
            "  Total woken dependents",
            wake_count > 0,
            f"count={wake_count:.0f}",
        )
    )
    results.append(
        check(
            "  Instructions added to IQ",
            insts_added > 0,
            f"count={insts_added:.0f}",
        )
    )
    results.append(
        check(
            "  Instructions executed",
            insts_executed > 0,
            f"count={insts_executed:.0f}",
        )
    )

    # ============================================================
    # 6. FENCE Synchronization
    # ============================================================
    print("\n=== 6. FENCE / Barrier ===")
    fence_count = get_val(stats, "fenceSyncCount")
    results.append(
        check(
            "  FENCE sync events", fence_count > 0, f"count={fence_count:.0f}"
        )
    )

    # ============================================================
    # 7. Committed Instructions
    # ============================================================
    print("\n=== 7. Committed Instructions ===")
    committed = get_val(stats, "commitStats0.numInsts")
    results.append(
        check(
            "  Committed instructions", committed > 0, f"count={committed:.0f}"
        )
    )
    results.append(
        check(
            "  Committed > 10000 (large test)",
            committed > 10000,
            f"count={committed:.0f}",
        )
    )

    # ============================================================
    # 8. Writeback Port Utilization
    # ============================================================
    print("\n=== 8. Writeback Ports ===")
    wb_fanout = get_val(stats, "wbFanout")
    results.append(
        check("  WB fanout > 0", wb_fanout > 0, f"fanout={wb_fanout:.4f}")
    )

    # ============================================================
    # 9. DIV-specific checks
    # ============================================================
    print("\n=== 9. DIV / IntDiv Checks ===")
    div_busy = get_val(stats, "statFuBusy::IntDiv")
    div_issued = get_val(stats, "issuedInstType_0::IntDiv")
    results.append(
        check("  IntDiv FU busy events", div_busy > 0, f"count={div_busy:.0f}")
    )
    results.append(
        check("  IntDiv issued", div_issued > 0, f"count={div_issued:.0f}")
    )

    # ============================================================
    # 10. IQ Empty / Pipeline Empty / Pipeline Stall
    # ============================================================
    print("\n=== 10. Pipeline State Cycles ===")
    iq_empty = get_val(stats, "iqEmptyCycles")
    pipe_empty = get_val(stats, "pipelineEmptyCycles")
    pipe_stall = get_val(stats, "pipelineStallCycles")
    backend_stall = get_val(stats, "backendStallCycles")

    print(f"  IQ empty cycles:        {iq_empty:.0f}")
    print(f"  Pipeline empty cycles:  {pipe_empty:.0f}")
    print(f"  Pipeline stall cycles:  {pipe_stall:.0f}")
    print(f"  Backend stall cycles:   {backend_stall:.0f}")

    results.append(
        check("  IQ empty occurred", iq_empty > 0, f"cycles={iq_empty:.0f}")
    )
    results.append(
        check(
            "  Pipeline empty occurred",
            pipe_empty > 0,
            f"cycles={pipe_empty:.0f}",
        )
    )
    results.append(
        check(
            "  Pipeline stall occurred",
            pipe_stall > 0,
            f"cycles={pipe_stall:.0f}",
        )
    )

    # ============================================================
    # Summary
    # ============================================================
    passed = sum(1 for r in results if r)
    total = len(results)
    print(f"\n{'='*50}")
    print(f" Results: {passed}/{total} checks passed")
    print(f"{'='*50}")

    if passed == total:
        print(" ALL CHECKS PASSED")
        sys.exit(0)
    else:
        print(f" {total - passed} CHECK(S) FAILED")
        sys.exit(1)


if __name__ == "__main__":
    main()
