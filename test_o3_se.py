"""Minimal SE test config for RiscvO3CPU with caches."""

import m5
from m5.objects import *


class L1Cache(Cache):
    assoc = 8
    tag_latency = 1
    data_latency = 1
    response_latency = 1
    mshrs = 16
    tgts_per_mshr = 20

    def connectBus(self, bus):
        self.mem_side = bus.cpu_side_ports

    def connectCPU(self, cpu):
        raise NotImplementedError


class L1ICache(L1Cache):
    size = "32KiB"

    def connectCPU(self, cpu):
        self.cpu_side = cpu.icache_port


class L1DCache(L1Cache):
    size = "32KiB"

    def connectCPU(self, cpu):
        self.cpu_side = cpu.dcache_port


class L2Cache(Cache):
    size = "512KiB"
    assoc = 16
    tag_latency = 10
    data_latency = 10
    response_latency = 1
    mshrs = 20
    tgts_per_mshr = 12

    def connectCPUSideBus(self, bus):
        self.cpu_side = bus.mem_side_ports

    def connectMemSideBus(self, bus):
        self.mem_side = bus.cpu_side_ports


class MySimpleMemory(SimpleMemory):
    latency = "1ns"


system = System()

system.workload = SEWorkload.init_compatible(
    "tests/test-progs/hello/bin/riscv/linux/hello"
)

system.clk_domain = SrcClockDomain()
system.clk_domain.clock = "1GHz"
system.clk_domain.voltage_domain = VoltageDomain()
system.mem_mode = "timing"
system.mem_ranges = [AddrRange("512MiB")]

system.cpu = RiscvO3CPU()
system.cpu.numPhysIntRegs = 80
system.cpu.numPhysFloatRegs = 48

system.cpu.l1d = L1DCache()
system.cpu.l1i = L1ICache()
system.l1_to_l2 = L2XBar()
system.l2cache = L2Cache()
system.membus = SystemXBar()
system.cpu.l1d.connectCPU(system.cpu)
system.cpu.l1d.connectBus(system.l1_to_l2)
system.cpu.l1i.connectCPU(system.cpu)
system.cpu.l1i.connectBus(system.l1_to_l2)
system.l2cache.connectCPUSideBus(system.l1_to_l2)
system.l2cache.connectMemSideBus(system.membus)

system.cpu.createInterruptController()

system.mem_ctrl = MySimpleMemory()
system.mem_ctrl.range = system.mem_ranges[0]
system.mem_ctrl.port = system.membus.mem_side_ports
system.system_port = system.membus.cpu_side_ports

process = Process()
process.cmd = ["tests/test-progs/hello/bin/riscv/linux/hello"]
system.cpu.workload = process
system.cpu.createThreads()

root = Root(full_system=False, system=system)
m5.instantiate()

exit_event = m5.simulate()

print("Exiting @ tick %i because %s." % (m5.curTick(), exit_event.getCause()))
if exit_event.getCause() != "exiting with last active thread context":
    exit(1)
