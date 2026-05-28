# Copyright (c) 2010, 2017, 2020, 2024-2025 Arm Limited
# All rights reserved.
#
# The license below extends only to copyright in the software and shall
# not be construed as granting a license to any other intellectual
# property including but not limited to intellectual property relating
# to a hardware implementation of the functionality of the software
# licensed hereunder.  You may use the software subject to the license
# terms below provided that you ensure that this notice is replicated
# unmodified and in its entirety in all distributions of the software,
# modified or unmodified, in source code or in binary form.
#
# Copyright (c) 2006-2007 The Regents of The University of Michigan
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met: redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer;
# redistributions in binary form must reproduce the above copyright
# notice, this list of conditions and the following disclaimer in the
# documentation and/or other materials provided with the distribution;
# neither the name of the copyright holders nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

from m5.defines import buildEnv
from m5.objects.FuncUnit import *
from m5.params import *
from m5.SimObject import SimObject

#
# C910-style 8-pipe latency model
#
# Pipe0 (IU ALU):   ADD/SUB/AND/OR/XOR/SLL/SRL/SRA/SLT  → 1 cycle, pipelined
# Pipe1 (IU ALU):   Same as Pipe0 + MLA (MUL/MADD)      → 1 cycle ALU, 3 cycle MLA
# Pipe2 (BJU):      BR/JAL/JALR/AUIPC                   → 2 cycles, pipelined
# Pipe3 (LSU Load): LB/LH/LW/LD/etc                     → 1 cycle (cache hit)
# Pipe4 (LSU Spec): FENCE/FENCE.I/CSRR*                 → 1 cycle (special)
# Pipe5 (LSU Store): SB/SH/SW/SD                         → 1 cycle (pipelined)
# Pipe6 (VFPU ALU): Vector ALU ops                      → 3 cycles, pipelined
# Pipe7 (VFPU MA):  Vector multiply-accumulate           → 5 cycles, pipelined


class IntALU(FUDesc):
    # Pipe0/Pipe1: IU ALU — 1 cycle latency, fully pipelined
    opList = [OpDesc(opClass="IntAlu", opLat=1, pipelined=True)]
    count = 6


class IntMultDiv(FUDesc):
    # Pipe1: IU MLA — MUL/MADD (3 cycles, pipelined)
    # Pipe0/Pipe1: IU DIV — DIV/REM (20 cycles, not pipelined)
    opList = [
        OpDesc(opClass="IntMult", opLat=3, pipelined=True),
        OpDesc(opClass="IntDiv", opLat=20, pipelined=False),
    ]

    count = 2


class FP_ALU(FUDesc):
    # Pipe6: VFPU ALU — scalar FP ALU ops, 3 cycles, pipelined
    opList = [
        OpDesc(opClass="FloatAdd", opLat=3, pipelined=True),
        OpDesc(opClass="FloatCmp", opLat=3, pipelined=True),
        OpDesc(opClass="FloatCvt", opLat=3, pipelined=True),
        OpDesc(opClass="Bf16Cvt", opLat=3, pipelined=True),
    ]
    count = 4


class FP_MultDiv(FUDesc):
    # Pipe7: VFPU MA — FloatMult/MultAcc (5 cycles, pipelined)
    # Pipe6/7: FloatDiv/Sqrt (not pipelined)
    opList = [
        OpDesc(opClass="FloatMult", opLat=5, pipelined=True),
        OpDesc(opClass="FloatMultAcc", opLat=5, pipelined=True),
        OpDesc(opClass="FloatMisc", opLat=3, pipelined=True),
        OpDesc(opClass="FloatDiv", opLat=12, pipelined=False),
        OpDesc(opClass="FloatSqrt", opLat=24, pipelined=False),
    ]
    count = 2


class SIMD_Unit(FUDesc):
    # Pipe6 (VFPU ALU): Vector ALU ops — 3 cycles, pipelined
    # Pipe7 (VFPU MA):  Vector Mult/MultAcc — 5 cycles, pipelined
    opList = [
        # Vector ALU → 3 cycles (Pipe6)
        OpDesc(opClass="SimdAdd", opLat=3, pipelined=True),
        OpDesc(opClass="SimdAddAcc", opLat=3, pipelined=True),
        OpDesc(opClass="SimdAlu", opLat=3, pipelined=True),
        OpDesc(opClass="SimdCmp", opLat=3, pipelined=True),
        OpDesc(opClass="SimdCvt", opLat=3, pipelined=True),
        OpDesc(opClass="SimdMisc", opLat=3, pipelined=True),
        OpDesc(opClass="SimdShift", opLat=3, pipelined=True),
        OpDesc(opClass="SimdShiftAcc", opLat=3, pipelined=True),
        OpDesc(opClass="SimdExt", opLat=3, pipelined=True),
        OpDesc(opClass="SimdFloatExt", opLat=3, pipelined=True),
        OpDesc(opClass="SimdConfig", opLat=3, pipelined=True),
        # Vector Multiply/Accumulate → 5 cycles (Pipe7)
        OpDesc(opClass="SimdMult", opLat=5, pipelined=True),
        OpDesc(opClass="SimdMultAcc", opLat=5, pipelined=True),
        OpDesc(opClass="SimdMatMultAcc", opLat=5, pipelined=True),
        OpDesc(opClass="SimdFloatMult", opLat=5, pipelined=True),
        OpDesc(opClass="SimdFloatMultAcc", opLat=5, pipelined=True),
        OpDesc(opClass="SimdFloatMatMultAcc", opLat=5, pipelined=True),
        # Vector Div/Sqrt → not pipelined
        OpDesc(opClass="SimdDiv", opLat=12, pipelined=False),
        OpDesc(opClass="SimdSqrt", opLat=24, pipelined=False),
        OpDesc(opClass="SimdFloatDiv", opLat=12, pipelined=False),
        OpDesc(opClass="SimdFloatSqrt", opLat=24, pipelined=False),
        # Vector FP ALU → 3 cycles
        OpDesc(opClass="SimdFloatAdd", opLat=3, pipelined=True),
        OpDesc(opClass="SimdFloatAlu", opLat=3, pipelined=True),
        OpDesc(opClass="SimdFloatCmp", opLat=3, pipelined=True),
        OpDesc(opClass="SimdFloatCvt", opLat=3, pipelined=True),
        OpDesc(opClass="SimdFloatMisc", opLat=3, pipelined=True),
        # Vector Reduce → 3 cycles
        OpDesc(opClass="SimdReduceAdd", opLat=3, pipelined=True),
        OpDesc(opClass="SimdReduceAlu", opLat=3, pipelined=True),
        OpDesc(opClass="SimdReduceCmp", opLat=3, pipelined=True),
        OpDesc(opClass="SimdFloatReduceAdd", opLat=3, pipelined=True),
        OpDesc(opClass="SimdFloatReduceCmp", opLat=3, pipelined=True),
        # Crypto extensions → 3 cycles
        OpDesc(opClass="SimdDotProd", opLat=3, pipelined=True),
        OpDesc(opClass="SimdAes", opLat=3, pipelined=True),
        OpDesc(opClass="SimdAesMix", opLat=3, pipelined=True),
        OpDesc(opClass="SimdSha1Hash", opLat=3, pipelined=True),
        OpDesc(opClass="SimdSha1Hash2", opLat=3, pipelined=True),
        OpDesc(opClass="SimdSha256Hash", opLat=3, pipelined=True),
        OpDesc(opClass="SimdSha256Hash2", opLat=3, pipelined=True),
        OpDesc(opClass="SimdShaSigma2", opLat=3, pipelined=True),
        OpDesc(opClass="SimdShaSigma3", opLat=3, pipelined=True),
        OpDesc(opClass="SimdSha3", opLat=3, pipelined=True),
        OpDesc(opClass="SimdSm4e", opLat=3, pipelined=True),
        OpDesc(opClass="SimdCrc", opLat=3, pipelined=True),
        # BF16 → varies
        OpDesc(opClass="SimdBf16Add", opLat=3, pipelined=True),
        OpDesc(opClass="SimdBf16Cmp", opLat=3, pipelined=True),
        OpDesc(opClass="SimdBf16Cvt", opLat=3, pipelined=True),
        OpDesc(opClass="SimdBf16DotProd", opLat=5, pipelined=True),
        OpDesc(opClass="SimdBf16MatMultAcc", opLat=5, pipelined=True),
        OpDesc(opClass="SimdBf16Mult", opLat=5, pipelined=True),
        OpDesc(opClass="SimdBf16MultAcc", opLat=5, pipelined=True),
    ]
    count = 4


class Matrix_Unit(FUDesc):
    # Pipe6/7: Matrix ops → 5 cycles, pipelined
    opList = [
        OpDesc(opClass="Matrix", opLat=5, pipelined=True),
        OpDesc(opClass="MatrixMov", opLat=3, pipelined=True),
        OpDesc(opClass="MatrixOP", opLat=5, pipelined=True),
    ]
    count = 1


class System_Unit(FUDesc):
    # Pipe4: LSU Special — FENCE/FENCE.I/CSRR* → 1 cycle
    opList = [OpDesc(opClass="System", opLat=1, pipelined=True)]
    count = 1


class PredALU(FUDesc):
    # Predicate ALU → 1 cycle, pipelined
    opList = [OpDesc(opClass="SimdPredAlu", opLat=1, pipelined=True)]
    count = 1


class ReadPort(FUDesc):
    # Pipe3: LSU Load — variable latency (cache hit 1 cycle)
    opList = [
        OpDesc(opClass="MemRead", opLat=1, pipelined=True),
        OpDesc(opClass="FloatMemRead", opLat=1, pipelined=True),
        OpDesc(opClass="SimdUnitStrideLoad", opLat=1, pipelined=True),
        OpDesc(opClass="SimdUnitStrideMaskLoad", opLat=1, pipelined=True),
        OpDesc(opClass="SimdUnitStrideSegmentedLoad", opLat=1, pipelined=True),
        OpDesc(opClass="SimdStridedLoad", opLat=1, pipelined=True),
        OpDesc(opClass="SimdIndexedLoad", opLat=1, pipelined=True),
        OpDesc(
            opClass="SimdUnitStrideFaultOnlyFirstLoad", opLat=1, pipelined=True
        ),
        OpDesc(
            opClass="SimdUnitStrideSegmentedFaultOnlyFirstLoad",
            opLat=1,
            pipelined=True,
        ),
        OpDesc(opClass="SimdWholeRegisterLoad", opLat=1, pipelined=True),
        OpDesc(opClass="SimdStrideSegmentedLoad", opLat=1, pipelined=True),
    ]
    count = 0


class WritePort(FUDesc):
    # Pipe5: LSU Store — 1-2 cycles, pipelined
    opList = [
        OpDesc(opClass="MemWrite", opLat=1, pipelined=True),
        OpDesc(opClass="FloatMemWrite", opLat=1, pipelined=True),
        OpDesc(opClass="SimdUnitStrideStore", opLat=1, pipelined=True),
        OpDesc(opClass="SimdUnitStrideMaskStore", opLat=1, pipelined=True),
        OpDesc(
            opClass="SimdUnitStrideSegmentedStore", opLat=1, pipelined=True
        ),
        OpDesc(opClass="SimdStridedStore", opLat=1, pipelined=True),
        OpDesc(opClass="SimdIndexedStore", opLat=1, pipelined=True),
        OpDesc(opClass="SimdWholeRegisterStore", opLat=1, pipelined=True),
        OpDesc(opClass="SimdStrideSegmentedStore", opLat=1, pipelined=True),
    ]
    count = 0


class RdWrPort(FUDesc):
    # Pipe3/Pipe5: LSU Load/Store — 1 cycle (pipelined), covers both read and write
    opList = [
        OpDesc(opClass="MemRead", opLat=1, pipelined=True),
        OpDesc(opClass="MemWrite", opLat=1, pipelined=True),
        OpDesc(opClass="FloatMemRead", opLat=1, pipelined=True),
        OpDesc(opClass="FloatMemWrite", opLat=1, pipelined=True),
        OpDesc(opClass="SimdUnitStrideLoad", opLat=1, pipelined=True),
        OpDesc(opClass="SimdUnitStrideStore", opLat=1, pipelined=True),
        OpDesc(opClass="SimdUnitStrideMaskLoad", opLat=1, pipelined=True),
        OpDesc(opClass="SimdUnitStrideMaskStore", opLat=1, pipelined=True),
        OpDesc(opClass="SimdUnitStrideSegmentedLoad", opLat=1, pipelined=True),
        OpDesc(
            opClass="SimdUnitStrideSegmentedStore", opLat=1, pipelined=True
        ),
        OpDesc(opClass="SimdStridedLoad", opLat=1, pipelined=True),
        OpDesc(opClass="SimdStridedStore", opLat=1, pipelined=True),
        OpDesc(opClass="SimdIndexedLoad", opLat=1, pipelined=True),
        OpDesc(opClass="SimdIndexedStore", opLat=1, pipelined=True),
        OpDesc(
            opClass="SimdUnitStrideFaultOnlyFirstLoad", opLat=1, pipelined=True
        ),
        OpDesc(
            opClass="SimdUnitStrideSegmentedFaultOnlyFirstLoad",
            opLat=1,
            pipelined=True,
        ),
        OpDesc(opClass="SimdWholeRegisterLoad", opLat=1, pipelined=True),
        OpDesc(opClass="SimdWholeRegisterStore", opLat=1, pipelined=True),
        OpDesc(opClass="SimdStrideSegmentedLoad", opLat=1, pipelined=True),
        OpDesc(opClass="SimdStrideSegmentedStore", opLat=1, pipelined=True),
    ]
    count = 4
