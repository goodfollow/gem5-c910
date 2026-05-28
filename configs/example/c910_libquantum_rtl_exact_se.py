# libquantum config — EXACT OpenC910 RTL parameters
# Uses the exact values from C910 RTL source code (C910_Parameters.md)
#
# Differences from c910_libquantum_se.py:
#   numPhysFloatRegs: 48 → 32 (RTL has 32 EREGs)
#   numPhysVecRegs:   48 → 64 (RTL has 64 VREGs)
#   numPhysVecPredRegs: 32 → 0 (doesn't exist in RTL)
#   numPhysMatRegs:   2 → 0 (doesn't exist in RTL)
#
# Usage:
#   build/RISCV/gem5.opt configs/example/c910_libquantum_rtl_exact_se.py

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
# CPU — EXACT OpenC910 RTL parameters
# ============================================================
system.cpu = RiscvO3CPU()
cpu = system.cpu

# Pipeline widths — from RTL source
cpu.fetchWidth = 3  # ct_ifu_top.v: 3 inst outputs
cpu.decodeWidth = 3  # ct_idu_top.v: 3 inst inputs
cpu.renameWidth = 4  # ct_idu_ir_dp.v: 4 preg paths
cpu.dispatchWidth = 4  # ct_rtu_rob.v: 4 create ports
cpu.issueWidth = 8  # 8 physical execution pipes
cpu.commitWidth = 3  # ct_rtu_rob.v: 3 retire ports

# ROB and IQ — from RTL
cpu.numROBEntries = 64  # ct_rtu_rob.v: 64 entries
cpu.numRobs = 1
cpu.renameToIEWDelay = 2
cpu.iewToCommitDelay = 1

# LSQ — from RTL
cpu.LQEntries = 16  # ct_lsu_lq.v:159
cpu.SQEntries = 12  # ct_lsu_sq_entry.v:614
cpu.LSQDepCheckShift = 4
cpu.LSQCheckLoads = True

# Multi-IQ (8-type) — from C910_IssueQueue_SPEC.md
cpu.aiq0Entries = 11
cpu.aiq1Entries = 11
cpu.biqEntries = 12
cpu.lsiqEntries = 16
cpu.sdiqEntries = 8
cpu.viq0Entries = 10
cpu.viq1Entries = 10
cpu.vmbEntries = 8

# PRF sizes — EXACT RTL values
cpu.numPhysIntRegs = 95  # ct_idu_rf_prf_pregfile.v: 95
cpu.numPhysFloatRegs = 32  # ct_idu_rf_prf_eregfile.v: 32 (NOT 48)
cpu.numPhysVecRegs = 64  # ct_idu_rf_prf_vregfile.v: 64 (NOT 48)
cpu.numPhysVecPredRegs = 0  # Doesn't exist in RTL
cpu.numPhysMatRegs = 0  # Doesn't exist in RTL
cpu.numPhysCCRegs = 0  # Doesn't exist in RTL

# Writeback widths — from RTL
cpu.pregWBWidth = 3  # ct_rtu_pst_preg.v: 3 ports
cpu.vregWBWidth = 3  # ct_rtu_pst_vreg.v: 3 ports
cpu.eregWBWidth = 2  # ct_rtu_pst_ereg.v: 2 ports
cpu.wbWidth = 3

# Branch predictor (same as other configs)
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
    "Beginning libquantum RTL-EXACT simulation (C910 RTL-accurate params)..."
)
exit_event = m5.simulate()
print(f"Simulation exited at tick {m5.curTick()}: {exit_event.getCause()}")
