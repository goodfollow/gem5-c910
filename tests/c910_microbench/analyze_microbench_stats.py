#!/usr/bin/env python3
"""
analyze_microbench_stats.py — 分析 gem5 stats.txt 中的 C910 微基准测试结果

用法：
    python3 analyze_microbench_stats.py <path/to/stats.txt>

读取 gem5 的 stats.txt 输出，提取 C910 相关统计项，对比 pass/fail 阈值，
输出结构化的测试报告。
"""

import re
import sys


def parse_stats(filepath):
    """Parse gem5 stats.txt and return a dict of stat_name -> value."""
    stats = {}
    with open(filepath) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            match = re.match(r"^([\w.:~-]+(?:\[[\d]+\])?)\s+([\d.e+-]+)", line)
            if match:
                name = match.group(1)
                value = float(match.group(2))
                stats[name] = value
    return stats


def check_pipe_issue(stats, prefix="system.cpu.iew.pipeIssueCount"):
    """Check pipe issue counts and report per-pipe coverage."""
    pipes = [
        ("pipe0_iu_alu", "Pipe0_IU_ALU"),
        ("pipe1_iu_alu_mla", "Pipe1_IU_MLA_DIV"),
        ("pipe2_bju", "Pipe2_BJU"),
        ("pipe3_lsu_load", "Pipe3_LSU_Load"),
        ("pipe4_lsu_special", "Pipe4_LSU_Special"),
        ("pipe5_lsu_store", "Pipe5_LSU_Store"),
        ("pipe6_vfpu_alu", "Pipe6_VFPU_ALU"),
        ("pipe7_vfpu_ma", "Pipe7_VFPU_MA"),
    ]
    results = {}
    for subname, name in pipes:
        key = f"{prefix}::{subname}"
        value = stats.get(key, 0)
        results[name] = {
            "count": int(value),
            "hit": value > 0,
        }
    return results


def check_stall_types(stats, prefix="system.cpu.iew.stallCycles"):
    """Check stall type histogram."""
    stall_names = [
        ("no_stall", "NO_STALL"),
        ("rob_full", "ROB_FULL"),
        ("iq_full", "IQ_FULL"),
        ("vmb_full", "VMB_FULL"),
        ("type_stall", "TYPE_STALL"),
        ("dispatch_stall", "DISPATCH_STALL"),
        ("is_stall", "IS_STALL"),
        ("div_stall", "DIV_STALL"),
        ("vdiv_stall", "VDIV_STALL"),
        ("backend_stall", "BACKEND_STALL"),
    ]
    results = {}
    for subname, name in stall_names:
        key = f"{prefix}::{subname}"
        value = stats.get(key, 0)
        results[name] = {
            "cycles": int(value),
            "active": value > 0,
        }
    return results


def check_ex_path(stats):
    """Check EX1/EX2 path statistics and EX2 latency histogram."""
    ex1 = stats.get("system.cpu.ex1ForwardInsts", 0)
    ex2 = stats.get("system.cpu.ex2WritebackInsts", 0)
    hist = {}
    for bucket in [
        "1-4",
        "5-8",
        "9-12",
        "13-16",
        "17-20",
        "21-24",
        "25-28",
        "29-32",
    ]:
        key = f"system.cpu.ex2LatencyHist::{bucket}"
        hist[bucket] = int(stats.get(key, 0))
    return {
        "ex1_forward": int(ex1),
        "ex2_writeback": int(ex2),
        "ratio": ex2 / (ex1 + ex2) if (ex1 + ex2) > 0 else 0,
        "ex2_hist": hist,
        "ex2_hist_min": int(
            stats.get("system.cpu.ex2LatencyHist::min_value", 0)
        ),
        "ex2_hist_max": int(
            stats.get("system.cpu.ex2LatencyHist::max_value", 0)
        ),
    }


def check_branch_mispredicts(stats):
    """Check branch misprediction statistics."""
    mispredicts = stats.get("system.cpu.iew.branchMispredicts", 0)
    return {
        "count": int(mispredicts),
        "active": mispredicts > 0,
    }


def check_fence_sync(stats):
    """Check FENCE synchronization count."""
    count = stats.get("system.cpu.iew.fenceSyncCount", 0)
    return {
        "count": int(count),
        "active": count > 0,
    }


def print_report(
    pipe_results,
    stall_results,
    ex_results,
    branch_results,
    fence_results,
    total_ticks,
):
    """Print structured test report."""
    print("=" * 60)
    print("  C910 IEW Micro-Benchmark Verification Report")
    print("=" * 60)

    # Pipe Coverage
    print("\n--- Pipe Issue Coverage ---")
    all_hit = True
    for name, info in pipe_results.items():
        status = "HIT" if info["hit"] else "MISS"
        if not info["hit"]:
            all_hit = False
        print(f"  {name:25s}: {info['count']:>10d}  [{status}]")
    print(
        f"  Overall: {'ALL PIPES COVERED' if all_hit else 'SOME PIPES MISSED'}"
    )

    # Stall Types
    print("\n--- Stall Type Histogram ---")
    for name, info in stall_results.items():
        if name == "NO_STALL":
            continue
        status = "ACTIVE" if info["active"] else "INACTIVE"
        print(f"  {name:25s}: {info['cycles']:>10d} cycles  [{status}]")

    # EX1/EX2 Path
    print("\n--- EX1/EX2 Path Distribution ---")
    print(f"  EX1 Forward Insts  : {ex_results['ex1_forward']:>10d}")
    print(f"  EX2 Writeback Insts: {ex_results['ex2_writeback']:>10d}")
    print(f"  EX2 Ratio          : {ex_results['ratio']:>10.4f}")
    print(
        f"  EX2 Latency Min    : {ex_results.get('ex2_hist_min', 0):>10d} cycles"
    )
    print(
        f"  EX2 Latency Max    : {ex_results.get('ex2_hist_max', 0):>10d} cycles"
    )
    hist = ex_results.get("ex2_hist", {})
    if hist:
        print("  EX2 Latency Histogram (cycles):")
        for bucket, count in hist.items():
            bar = "#" * min(count, 50)
            print(f"    {bucket:6s}: {count:>6d}  {bar}")

    # Branch Mispredicts
    print("\n--- Branch Mispredict ---")
    status = "DETECTED" if branch_results["active"] else "NOT DETECTED"
    print(f"  Mispredicts: {branch_results['count']:>10d}  [{status}]")

    # FENCE Sync
    print("\n--- FENCE Synchronization ---")
    status = "DETECTED" if fence_results["active"] else "NOT DETECTED"
    print(f"  fenceSyncCount: {fence_results['count']:>10d}  [{status}]")

    # Total Ticks
    print(f"\n--- Total Ticks: {total_ticks:>15.0f} ---")

    # Summary
    print("\n" + "=" * 60)
    print("  Verification Summary")
    print("=" * 60)

    checks = [
        (
            "Pipe0 (IU ALU)",
            pipe_results.get("Pipe0_IU_ALU", {}).get("hit", False),
        ),
        (
            "Pipe1 (IU MLA/DIV)",
            pipe_results.get("Pipe1_IU_MLA_DIV", {}).get("hit", False),
        ),
        ("Pipe2 (BJU)", pipe_results.get("Pipe2_BJU", {}).get("hit", False)),
        (
            "Pipe3 (LSU Load)",
            pipe_results.get("Pipe3_LSU_Load", {}).get("hit", False),
        ),
        (
            "Pipe4 (LSU Special)",
            pipe_results.get("Pipe4_LSU_Special", {}).get("hit", False),
        ),
        (
            "Pipe5 (LSU Store)",
            pipe_results.get("Pipe5_LSU_Store", {}).get("hit", False),
        ),
        (
            "Pipe6 (VFPU ALU)",
            pipe_results.get("Pipe6_VFPU_ALU", {}).get("hit", False),
        ),
        (
            "Pipe7 (VFPU MA)",
            pipe_results.get("Pipe7_VFPU_MA", {}).get("hit", False),
        ),
        (
            "ROB_FULL Stall",
            stall_results.get("ROB_FULL", {}).get("active", False),
        ),
        (
            "IQ_FULL Stall",
            stall_results.get("IQ_FULL", {}).get("active", False),
        ),
        ("DIV_STALL", stall_results.get("DIV_STALL", {}).get("active", False)),
        ("Branch Mispredict", branch_results["active"]),
        ("FENCE Sync", fence_results["active"]),
        ("EX1/EX2 Path Split", ex_results["ex2_writeback"] > 0),
    ]

    passed = sum(1 for _, ok in checks if ok)
    total = len(checks)

    for name, ok in checks:
        status = "PASS" if ok else "FAIL"
        print(f"  [{status:4s}] {name}")

    print(f"\n  {passed}/{total} checks passed")
    if passed == total:
        print("  >>> ALL CHECKS PASSED - C910 IEW alignment verified <<<")
    elif passed >= total * 0.7:
        print("  >>> MOST CHECKS PASSED - minor gaps remain <<<")
    else:
        print("  >>> SIGNIFICANT GAPS - more micro-benchmarks needed <<<")
    print("=" * 60)


def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <stats.txt>")
        sys.exit(1)

    filepath = sys.argv[1]
    stats = parse_stats(filepath)

    if not stats:
        print(f"Error: No stats found in {filepath}")
        sys.exit(1)

    pipe_results = check_pipe_issue(stats)
    stall_results = check_stall_types(stats)
    ex_results = check_ex_path(stats)
    branch_results = check_branch_mispredicts(stats)
    fence_results = check_fence_sync(stats)

    total_ticks = stats.get("simTicks", 0)

    print_report(
        pipe_results,
        stall_results,
        ex_results,
        branch_results,
        fence_results,
        total_ticks,
    )


if __name__ == "__main__":
    main()
