# C910-like IEW full test SE config
# Usage:
#   build/RISCV/gem5.opt configs/example/c910_full_test_se.py

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
# CPU — C910-like O3 parameters
# ============================================================
system.cpu = RiscvO3CPU()
cpu = system.cpu

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
# Caches
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

# Connect CPU cache ports to membus (simple, no L2)
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

# System port
system.system_port = system.membus.cpu_side_ports

# ============================================================
# Workload
# ============================================================
binary = os.path.join(
    m5_root,
    "tests",
    "test-progs",
    "c910-full-test",
    "bin",
    "riscv",
    "linux",
    "c910_iew_full_test",
)

system.workload = SEWorkload.init_compatible(binary)

process = Process()
process.cmd = [binary]
process.executable = binary
process.pid = 100

cpu.workload = process
cpu.createThreads()
cpu.createInterruptController()

# ============================================================
# Redirect paths
# ============================================================
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

print("Beginning C910 IEW Full Test simulation...")
exit_event = m5.simulate()
print(f"Simulation exited at tick {m5.curTick()}: {exit_event.getCause()}")
