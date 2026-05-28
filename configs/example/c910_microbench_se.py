# C910-like IEW micro-benchmark SE config
# Usage:
#   build/RISCV/gem5.opt configs/example/c910_microbench_se.py
#
# Replicates the exact C910-like O3CPU parameters from the previous smoke test

import os
import sys

# Add gem5 source to path so we can import the config components
m5_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(m5_root, "configs", "common"))
sys.path.insert(0, os.path.join(m5_root, "configs", "example", "common"))

import m5
from m5.objects import *

# ============================================================
# System
# ============================================================
system = System()
system.clk_domain = SrcClockDomain(
    clock="1GHz", voltage_domain=VoltageDomain()
)
system.mem_mode = "timing"
system.mem_ranges = [AddrRange("512MB")]

from m5.objects.FuncUnit import (
    FP_ALU,
    FP_MultDiv,
    IntALU,
    IntMultDiv,
    Matrix_Unit,
    PredALU,
    RdWrPort,
    SIMD_Unit,
    System_Unit,
)

# ============================================================
# CPU — C910-like O3 parameters (matching config.ini from smoke test)
# ============================================================
from m5.objects.FUPool import DefaultFUPool

cpu = BaseO3CPU()

# Pipeline widths
cpu.fetchWidth = 3
cpu.decodeWidth = 3
cpu.renameWidth = 4
cpu.dispatchWidth = 4
cpu.issueWidth = 8
cpu.commitWidth = 3

# ROB and IQ
cpu.numROBEntries = 64
cpu.numRobs = 1
cpu.renameToIEWDelay = 2
cpu.iewToCommitDelay = 1

# LSQ
cpu.LQEntries = 16
cpu.SQEntries = 12
cpu.LSQDepCheckShift = 4
cpu.LSQCheckLoads = True

# Multi-IQ (C910 8-type)
cpu.aiq0Entries = 11
cpu.aiq1Entries = 11
cpu.biqEntries = 12
cpu.lsiqEntries = 16
cpu.sdiqEntries = 8
cpu.viq0Entries = 10
cpu.viq1Entries = 10
cpu.vmbEntries = 8

# PRF sizes
cpu.numPhysIntRegs = 95
cpu.numPhysFloatRegs = 48
cpu.numPhysVecRegs = 48
cpu.numPhysVecPredRegs = 32
cpu.numPhysMatRegs = 2
cpu.numPhysCCRegs = 0

# Writeback widths
cpu.pregWBWidth = 3
cpu.vregWBWidth = 3
cpu.eregWBWidth = 2
cpu.wbWidth = 3

# Branch predictor
cpu.branchPred = TournamentBP(
    localPredictorSize=64,
    localCtrBits=2,
    localHistoryTableSize=512,
    localHistoryBits=6,
    globalPredictorSize=16384,
    globalCtrBits=2,
    globalHistoryBits=14,
    choicePredictorSize=8192,
    choiceCtrBits=2,
    numThreads=1,
    btb=SimpleBTB(
        numEntries=4096,
        tagBits=16,
        assoc=4,
    ),
    ras=ReturnAddrStack(
        numEntries=8,
    ),
)

# CPU clock
cpu_clk = SrcClockDomain(clock="1GHz", voltage_domain=VoltageDomain())
cpu.clk_domain = cpu_clk

# ============================================================
# Memory
# ============================================================
system.mem_ctrls = [
    MemCtrl(
        clk_domain=cpu_clk,
        dram=DDR4_2400_4x16(),
    )
]
system.mem_ctrls[0].dram.range = system.mem_ranges[0]

# ============================================================
# Caches
# ============================================================
l1d_cache = Cache(
    assoc=2,
    tag_latency=2,
    data_latency=2,
    response_latency=2,
    mshrs=16,
    tgts_per_mshr=8,
    size="64kB",
    system=system,
)
l1i_cache = Cache(
    assoc=2,
    tag_latency=2,
    data_latency=2,
    response_latency=2,
    mshrs=16,
    tgts_per_mshr=8,
    size="32kB",
    system=system,
)

cpu.dcache = l1d_cache
cpu.icache = l1i_cache

# ============================================================
# L2 cache
# ============================================================
l2_cache = Cache(
    assoc=8,
    tag_latency=12,
    data_latency=12,
    response_latency=2,
    mshrs=16,
    tgts_per_mshr=8,
    writeback_buffers=8,
    size="256kB",
    system=system,
)
cpu.dcache.mem_side = l2_cache.cpu_side
cpu.icache.mem_side = l2_cache.cpu_side

# ============================================================
# Memory bus
# ============================================================
system.membus = SystemXBar()
l2_cache.mem_side = system.membus.cpu_side_ports

# Connect CPU
cpu.connectCpuPorts(system.membus)

# ============================================================
# Workload
# ============================================================
binary = os.path.join(
    m5_root,
    "tests",
    "test-progs",
    "c910-microbench",
    "bin",
    "riscv",
    "linux",
    "c910_microbench",
)
if not os.path.exists(binary):
    # Fallback: try relative path
    binary = os.path.join(os.getcwd(), "c910_microbench", "c910_microbench")

process = Process()
process.cmd = [binary]
process.executable = binary

# Use BaseO3CPU directly (not RiscvO3CPU) to match smoke test config
# Manually set ISA/decoder/MMU
cpu.workload = [process]

# ============================================================
# Redirect paths (to suppress host fs access)
# ============================================================
for i in range(3):
    rp = RedirectPath(
        app_path="/proc",
        host_path=os.path.join(m5_root, "tests", "test-progs", "hello"),
    )
    setattr(system, f"redirect_paths{i}", rp)

# ============================================================
# Root
# ============================================================
root = Root(full_system=False, system=system)
m5.instantiate()

print("Beginning simulation...")
exit_event = m5.simulate()
print(f"Simulation exited at tick {m5.curTick()}: {exit_event.getCause()}")
