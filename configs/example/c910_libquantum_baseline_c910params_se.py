# Baseline libquantum config — Original gem5 O3 code + C910-like parameters
# Tests: do C910-like pipeline WIDTHS alone affect performance?
# (Without the IEW code modifications — single IQ, no EX1/EX2, no typed IQs)
#
# Usage:
#   build/RISCV/gem5.opt configs/example/c910_libquantum_baseline_c910params_se.py

import os
import sys

m5_root = os.path.abspath(
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
)

import m5
from m5.objects import *
from m5.objects.RiscvCPU import RiscvO3CPU

# ============================================================
# System
# ============================================================
system = System()

system.clk_domain = SrcClockDomain()
system.clk_domain.clock = "1GHz"
system.clk_domain.voltage_domain = VoltageDomain()

system.mem_mode = "timing"
system.mem_ranges = [AddrRange("512MiB")]

system.membus = SystemXBar()

# ============================================================
# CPU — ORIGINAL gem5 code + C910-like pipeline parameters
# ============================================================
system.cpu = RiscvO3CPU()
cpu = system.cpu

# C910-like pipeline widths (same as c910_libquantum_se.py)
cpu.fetchWidth = 3
cpu.decodeWidth = 3
cpu.renameWidth = 4
cpu.dispatchWidth = 4
cpu.issueWidth = 8
cpu.commitWidth = 3

# ROB (same as C910)
cpu.numROBEntries = 64
cpu.numRobs = 1
cpu.renameToIEWDelay = 2
cpu.iewToCommitDelay = 1

# LSQ (C910 values)
cpu.LQEntries = 16
cpu.SQEntries = 12

# PRF (C910 values)
cpu.numPhysIntRegs = 95
cpu.numPhysFloatRegs = 48
cpu.numPhysVecRegs = 48
cpu.numPhysVecPredRegs = 32
cpu.numPhysMatRegs = 2
cpu.numPhysCCRegs = 0

# Branch predictor (same for all configs)
cpu.branchPred = BranchPredictor(
    conditionalBranchPred=TournamentBP(
        localPredictorSize=64,
        localCtrBits=2,
        localHistoryTableSize=512,
        globalPredictorSize=16384,
        globalCtrBits=2,
        choicePredictorSize=8192,
        choiceCtrBits=2,
        speculativeHistUpdate=True,
        btb=SimpleBTB(
            numEntries=4096,
            tagBits=16,
            associativity=4,
        ),
        ras=ReturnAddrStack(
            numEntries=8,
        ),
    ),
)

# ============================================================
# Caches (same as other configs)
# ============================================================
cpu.icache = Cache(
    assoc=2,
    tag_latency=2,
    data_latency=2,
    response_latency=2,
    mshrs=16,
    tgts_per_mshr=8,
    size="64KiB",
)
cpu.dcache = Cache(
    assoc=2,
    tag_latency=2,
    data_latency=2,
    response_latency=2,
    mshrs=16,
    tgts_per_mshr=8,
    size="64KiB",
)

cpu.icache.mem_side = system.membus.cpu_side_ports
cpu.dcache.mem_side = system.membus.cpu_side_ports
cpu.icache_port = cpu.icache.cpu_side
cpu.dcache_port = cpu.dcache.cpu_side

# ============================================================
# Memory
# ============================================================
system.mem_ctrl = MemCtrl()
system.mem_ctrl.dram = DDR3_1600_8x8()
system.mem_ctrl.dram.range = system.mem_ranges[0]
system.mem_ctrl.port = system.membus.mem_side_ports

system.system_port = system.membus.cpu_side_ports

# ============================================================
# Workload — SAME binary
# ============================================================
binary = os.path.join(
    m5_root,
    "tests",
    "test-progs",
    "c910-libquantum",
    "bin",
    "riscv",
    "linux",
    "shor",
)

system.workload = SEWorkload.init_compatible(binary)

process = Process()
process.cmd = [binary, "15"]
process.executable = binary
process.pid = 100

cpu.workload = process
cpu.createThreads()
cpu.createInterruptController()

for i in range(3):
    rp = RedirectPath(
        app_path="/proc",
        host_paths=[os.path.join(m5_root, "tests", "test-progs", "hello")],
    )
    setattr(system, f"redirect_paths{i}", rp)

# ============================================================
# Root
# ============================================================
root = Root(full_system=False, system=system)
m5.instantiate()

print(
    "Beginning libquantum BASELINE+C910params simulation (original code, C910 widths)..."
)
exit_event = m5.simulate()
print(f"Simulation exited at tick {m5.curTick()}: {exit_event.getCause()}")
