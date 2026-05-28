import argparse

import m5
from m5.objects import *

parser = argparse.ArgumentParser()
parser.add_argument("-c", "--cpu-type", type=str, default="O3CPU")
parser.add_argument("--num-cpus", type=int, default=1)
parser.add_argument("--kernel", type=str, required=True)
parser.add_argument("-m", "--max-ticks", type=int, default=100000000)
parser.add_argument("-P", "--param", action="append", default=[])
parser.add_argument("--caches", action="store_true")

args = parser.parse_args()
np = args.num_cpus

system = System()
system.mem_ranges = [AddrRange(start=0x80000000, size="512MB")]
system.workload = RiscvBareMetal(bootloader=args.kernel)

system.clk_domain = SrcClockDomain(
    clock="3GHz", voltage_domain=VoltageDomain()
)
system.cpu_clk_domain = SrcClockDomain(
    clock="3GHz", voltage_domain=VoltageDomain()
)
if args.cpu_type == "O3CPU":
    system.mem_mode = "timing"
else:
    system.mem_mode = "atomic"

system.membus = SystemXBar()
system.system_port = system.membus.cpu_side_ports
system.iobus = IOXBar()

system.platform = HiFive()
system.platform.rtc = RiscvRTC(frequency=Frequency("100MHz"))
system.platform.clint.int_pin = system.platform.rtc.int_pin

system.iobus.cpu_side_ports = system.platform.pci_host.up_request_port()
system.iobus.mem_side_ports = system.platform.pci_host.up_response_port()
system.platform.pci_bus.cpu_side_ports = (
    system.platform.pci_host.down_request_port()
)
system.platform.pci_bus.default = system.platform.pci_host.down_response_port()
system.platform.pci_bus.config_error_port = (
    system.platform.pci_host.config_error.pio
)

system.platform.attachOnChipIO(system.membus)
system.platform.attachOffChipIO(system.iobus)
system.platform.attachPlic()
system.platform.setNumCores(np)

system.iobridge = Bridge(delay="50ns", ranges=system.mem_ranges)
system.iobridge.cpu_side_port = system.iobus.mem_side_ports
system.iobridge.mem_side_port = system.membus.cpu_side_ports

# Memory controller
if args.cpu_type == "O3CPU":
    system.mem_ctrl = MemCtrl()
    system.mem_ctrl.dram = DDR3_1600_8x8(
        range=AddrRange(start=0x80000000, size="512MB")
    )
    system.membus.mem_side_ports = system.mem_ctrl.port
else:
    system.mem_ctrl = SimpleMemory(
        range=AddrRange(start=0x80000000, size="512MB")
    )
    system.membus.mem_side_ports = system.mem_ctrl.port

CPUClass = O3CPU if args.cpu_type == "O3CPU" else AtomicSimpleCPU

system.cpu = [
    CPUClass(clk_domain=system.cpu_clk_domain, cpu_id=i) for i in range(np)
]

for i in range(np):
    system.cpu[i].createInterruptController()
    system.cpu[i].createThreads()

    # Connect CPU ports to memory bus
    if args.caches:
        icache = L1ICache(size="32kB")
        dcache = L1DCache(size="32kB")
        system.cpu[i].icache_port = icache.cpu_side
        system.cpu[i].dcache_port = dcache.cpu_side
        icache.mem_side = system.membus.cpu_side_ports
        dcache.mem_side = system.membus.cpu_side_ports
    else:
        system.cpu[i].icache_port = system.membus.cpu_side_ports
        system.cpu[i].dcache_port = system.membus.cpu_side_ports

uncacheable_range = [
    *system.platform._on_chip_ranges(),
    *system.platform._off_chip_ranges(),
]
for cpu in system.cpu:
    cpu.mmu.pma_checker = PMAChecker(uncacheable=uncacheable_range)

for p in args.param:
    exec(f"system.{p}", globals(), {"system": system})

root = Root(full_system=True, system=system)

m5.instantiate()

print("**** REAL SIMULATION ****")
exit_event = m5.simulate(args.max_ticks)
print(f"Exiting @ tick {m5.curTick()} because {exit_event.getCause()}")
