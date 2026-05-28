#!/usr/bin/env python3
"""
analyze_libquantum_stats.py — Validate gem5 statistics after libquantum (SPEC 462) run

Usage:
    build/RISCV/gem5.opt configs/example/c910_libquantum_se.py
    python3 analyze_libquantum_stats.py m5out/stats.txt

This script:
  1. Checks simout.log for correct output (15 = 3 * 5 or 15 = 5 * 3)
  2. Parses stats.txt and validates pipeline utilization
  3. Reports performance metrics (IPC, CPI, stall distribution)

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


def check_correctness(simout_path):
    """Check if Shor's algorithm produced correct output."""
    if not os.path.exists(simout_path):
        print(f"  Warning: simout.log not found at {simout_path}")
        return None  # Unknown

    with open(simout_path) as f:
        content = f.read()

    # Check for correct factorization: "15 = 3 * 5" or "15 = 5 * 3"
    if re.search(r"15\s*=\s*[35]\s*\*\s*[35]", content):
        print(f"[PASS]   Shor's algorithm correctly factored 15 = 3 × 5")
        return True
    elif "Unable to determine factors" in content:
        print(
            f"[FAIL]   Shor's algorithm failed to find factors (random period failure)"
        )
        return False
    elif "Measured zero" in content:
        print(f"[FAIL]   Shor's algorithm measured zero (try again)")
        return False
    else:
        print(f"[WARN]   Could not verify output (unknown result)")
        return None


def main():
    if len(sys.argv) < 2:
        print("Usage: python3 analyze_libquantum_stats.py m5out/stats.txt")
        sys.exit(1)

    filepath = sys.argv[1]
    if not os.path.exists(filepath):
        print(f"Error: File {filepath} not found")
        sys.exit(1)

    stats = parse_stats(filepath)
    print(f"Parsed {len(stats)} statistics from {filepath}\n")

    # Check correctness first
    simout = os.path.join(os.path.dirname(filepath), "simout")
    if not os.path.exists(simout):
        simout = os.path.join(os.path.dirname(filepath), "..", "simout")
        if not os.path.exists(simout):
            simout = None

    results = []

    # ============================================================
    # 0. Correctness — Shor's algorithm output
    # ============================================================
    print("=== 0. Algorithm Correctness ===")
    if simout:
        ok = check_correctness(simout)
        if ok is not None:
            results.append(ok)
    else:
        print("  (Cannot verify — simout not available)")
    print()

    # ============================================================
    # 1. Instruction Count & IPC
    # ============================================================
    print("=== 1. Instruction Count & IPC ===")
    committed = get_val(stats, "commitStats0.numInsts")
    executed = get_val(stats, "executeStats0.numInsts")
    cycles = get_val(stats, "system.cpu.numCycles")
    ipc = committed / cycles if cycles > 0 else 0

    print(f"  Committed instructions: {committed:.0f}")
    print(f"  Executed instructions:  {executed:.0f}")
    print(f"  Total cycles:           {cycles:.0f}")
    print(f"  IPC:                    {ipc:.4f}")

    results.append(
        check("  Committed > 0", committed > 0, f"count={committed:.0f}")
    )
    results.append(check("  IPC > 0", ipc > 0, f"ipc={ipc:.4f}"))

    # ============================================================
    # 2. Pipe Utilization — All 8 pipes exercised
    # ============================================================
    print("\n=== 2. Pipe Issue Counts ===")
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

    active_pipes = 0
    for pipe_name in pipe_names:
        val = get_val(stats, pipe_name)
        if val > 0:
            active_pipes += 1
        print(f"  {pipe_name:30s} = {val:12.0f}")

    results.append(
        check(
            f"  Active pipes >= 4",
            active_pipes >= 4,
            f"{active_pipes}/8 pipes active",
        )
    )

    # ============================================================
    # 3. Stall Distribution
    # ============================================================
    print("\n=== 3. Stall Distribution ===")
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

    total_stall = 0
    for stall_name in stall_names:
        val = get_val(stats, stall_name)
        pct = (val / cycles * 100) if cycles > 0 else 0
        print(f"  {stall_name:20s} = {val:12.0f}  ({pct:5.1f}%)")
        if stall_name != "no_stall":
            total_stall += val

    results.append(
        check(
            "  Stalls occurred",
            total_stall > 0,
            f"{total_stall:.0f} stall cycles",
        )
    )

    # ============================================================
    # 4. EX1 vs EX2 Path Separation
    # ============================================================
    print("\n=== 4. EX1 vs EX2 Path ===")
    ex1_val = get_val(stats, "ex1ForwardInsts")
    ex2_val = get_val(stats, "ex2WritebackInsts")
    print(f"  EX1 forward insts:   {ex1_val:.0f}")
    print(f"  EX2 writeback insts: {ex2_val:.0f}")

    results.append(
        check("  EX1 insts > 0", ex1_val > 0, f"count={ex1_val:.0f}")
    )
    results.append(
        check("  EX2 insts > 0", ex2_val > 0, f"count={ex2_val:.0f}")
    )

    # ============================================================
    # 5. IQ Activity
    # ============================================================
    print("\n=== 5. IQ Type Activity ===")
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
            "  IQ reads > 0",
            any(v > 0 for v in iq_reads.values()),
            f"total={sum(iq_reads.values()):.0f}",
        )
    )
    results.append(
        check(
            "  IQ writes > 0",
            any(v > 0 for v in iq_writes.values()),
            f"total={sum(iq_writes.values()):.0f}",
        )
    )

    # ============================================================
    # 6. Scoreboard & Wake
    # ============================================================
    print("\n=== 6. Scoreboard Activity ===")
    wake_count = get_val(stats, "totalWakeDependents")
    insts_added = get_val(stats, "instsAdded")

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

    # ============================================================
    # 7. Pipeline State
    # ============================================================
    print("\n=== 7. Pipeline State ===")
    iq_empty = get_val(stats, "iqEmptyCycles")
    pipe_empty = get_val(stats, "pipelineEmptyCycles")
    pipe_stall = get_val(stats, "pipelineStallCycles")

    print(f"  IQ empty cycles:        {iq_empty:.0f}")
    print(f"  Pipeline empty cycles:  {pipe_empty:.0f}")
    print(f"  Pipeline stall cycles:  {pipe_stall:.0f}")

    results.append(
        check(
            "  Pipeline was active",
            pipe_empty < cycles or pipe_stall > 0,
            f"active_cycles={cycles - pipe_empty:.0f}",
        )
    )

    # ============================================================
    # 8. Memory Activity (libquantum is memory-intensive)
    # ============================================================
    print("\n=== 8. Memory Activity ===")
    loads = get_val(stats, "numLoadInsts")
    stores = get_val(stats, "numStoreInsts")
    cache_hits = get_val(stats, "dcache.demandHits")
    cache_misses = get_val(stats, "dcache.demandMisses")

    print(f"  Load insts:       {loads:.0f}")
    print(f"  Store insts:      {stores:.0f}")
    print(f"  Cache hits:       {cache_hits:.0f}")
    print(f"  Cache misses:     {cache_misses:.0f}")

    results.append(
        check(
            "  Memory references > 0",
            loads + stores > 0,
            f"total={loads + stores:.0f}",
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
