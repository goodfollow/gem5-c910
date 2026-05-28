# Baseline libquantum config — Original gem5 O3 CPU (no C910 IEW modifications)
# Uses default BaseO3CPU pipeline parameters for comparison with C910-like config
#
# Usage:
#   build/RISCV/gem5.opt configs/example/c910_libquantum_baseline_se.py

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
# CPU — ORIGINAL gem5 O3 parameters (no C910 modifications)
# ============================================================
system.cpu = RiscvO3CPU()
cpu = system.cpu

# Original defaults (from BaseO3CPU.py)
# — Only override what we need to match the C910 config's cache/memory settings
# — Pipeline widths use ORIGINAL gem5 defaults (fetchWidth=8, etc.)

# ROB — original default is 64 (same as C910)
cpu.numROBEntries = 64
cpu.numRobs = 1
cpu.renameToIEWDelay = 2
cpu.iewToCommitDelay = 1

# LSQ — original defaults
cpu.LQEntries = 32
cpu.SQEntries = 32

# PRF — original defaults
cpu.numPhysIntRegs = 128
cpu.numPhysFloatRegs = 128
cpu.numPhysVecRegs = 256
cpu.numPhysVecPredRegs = 32
cpu.numPhysMatRegs = 2
cpu.numPhysCCRegs = 0

# ============================================================
# Branch predictor (same as C910 config for fair comparison)
# ============================================================
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
# Caches (SAME as C910 config for fair comparison)
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

# Connect CPU cache ports to membus
cpu.icache.mem_side = system.membus.cpu_side_ports
cpu.dcache.mem_side = system.membus.cpu_side_ports
cpu.icache_port = cpu.icache.cpu_side
cpu.dcache_port = cpu.dcache.cpu_side

# ============================================================
# Memory (SAME as C910 config)
# ============================================================
system.mem_ctrl = MemCtrl()
system.mem_ctrl.dram = DDR3_1600_8x8()
system.mem_ctrl.dram.range = system.mem_ranges[0]
system.mem_ctrl.port = system.membus.mem_side_ports

system.system_port = system.membus.cpu_side_ports

# ============================================================
# Workload — libquantum shor (factor N=15) — SAME binary
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

print("Beginning libquantum BASELINE simulation (original O3 defaults)...")
exit_event = m5.simulate()
print(f"Simulation exited at tick {m5.curTick()}: {exit_event.getCause()}")
