/*
 * Copyright (c) 2025 Arm Limited
 * All rights reserved
 *
 * The license below extends only to copyright in the software and shall
 * not be construed as granting a license to any other intellectual
 * property including but not limited to intellectual property relating
 * to a hardware implementation of the functionality of the software
 * licensed hereunder.  You may use the software subject to the license
 * terms below provided that you ensure that this notice is replicated
 * unmodified and in its entirety in all distributions of the software,
 * modified or unmodified, in source code or in binary form.
 *
 * Copyright (c) 2005-2006 The Regents of The University of Michigan
 * Copyright (c) 2013 Advanced Micro Devices, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef __CPU_O3_SCOREBOARD_HH__
#define __CPU_O3_SCOREBOARD_HH__

#include <cassert>
#include <vector>

#include "base/compiler.hh"
#include "base/logging.hh"
#include "base/trace.hh"
#include "cpu/reg_class.hh"
#include "debug/Scoreboard.hh"

namespace gem5
{

namespace o3
{

/**
 * Implements a scoreboard to track register readiness across multiple
 * independent physical register files (PRFs), aligned with OpenC910's
 * 3-PRF architecture (PREG/VREG/EREG).
 *
 * Register class routing:
 *   - IntRegClass       → pregScoreBoard (integer PRF)
 *   - FloatRegClass     → eregScoreBoard (scalar FP PRF)
 *   - VecRegClass       → vregScoreBoard (vector PRF)
 *   - VecPredRegClass   → otherScoreBoard (predicate registers)
 *   - VecElemClass      → otherScoreBoard (vector element registers)
 *   - MatRegClass       → otherScoreBoard (matrix registers)
 *   - CCRegClass        → otherScoreBoard (condition code registers)
 */
class Scoreboard
{
  private:
    /** The object name, for DPRINTF. */
    const std::string _name;

    /** Integer PRF scoreboard (PREG) — e.g., 95 registers for RISC-V. */
    std::vector<bool> pregScoreBoard;
    unsigned _numPregs;

    /** Scalar FP PRF scoreboard (EREG) — e.g., 32 registers. */
    std::vector<bool> eregScoreBoard;
    unsigned _numEregs;

    /** Vector PRF scoreboard (VREG) — e.g., 64 registers. */
    std::vector<bool> vregScoreBoard;
    unsigned _numVregs;

    /** Scoreboard for other renamable register classes
     *  (VecPred, VecElem, Mat, CC). Indexed by flatIndex(). */
    std::vector<bool> otherScoreBoard;
    unsigned _numOtherRegs;

    /** Total number of physical registers (sum of all above). */
    GEM5_CLASS_VAR_USED unsigned _numPhysRegs;

  public:
    /** Constructs a scoreboard with separate PRF partitions. */
    Scoreboard(const std::string &_my_name,
               unsigned _numPregs, unsigned _numEregs,
               unsigned _numVregs, unsigned _numOtherRegs);

    /** Destructor. */
    ~Scoreboard() {}

    /** Returns the name of the scoreboard. */
    std::string name() const { return _name; }

    /** Returns the total number of physical registers. */
    unsigned
    totalNumPhysRegs() const
    {
        return _numPhysRegs;
    }

    /** Returns the number of integer PRF registers. */
    unsigned
    numPregs() const
    {
        return _numPregs;
    }

    /** Returns the number of scalar FP PRF registers. */
    unsigned
    numEregs() const
    {
        return _numEregs;
    }

    /** Returns the number of vector PRF registers. */
    unsigned
    numVregs() const
    {
        return _numVregs;
    }

    /** Returns the number of other PRF registers. */
    unsigned
    numOtherRegs() const
    {
        return _numOtherRegs;
    }

    /** Checks if the register is ready. */
    bool
    getReg(PhysRegIdPtr phys_reg) const
    {
        if (phys_reg->isAlwaysReady()) {
            return true;
        }

        switch (phys_reg->classValue()) {
          case IntRegClass:
            assert(phys_reg->index() < _numPregs);
            return pregScoreBoard[phys_reg->index()];
          case FloatRegClass:
            assert(phys_reg->index() < _numEregs);
            return eregScoreBoard[phys_reg->index()];
          case VecRegClass:
            assert(phys_reg->index() < _numVregs);
            return vregScoreBoard[phys_reg->index()];
          default:
            // VecPred, VecElem, Mat, CC → other scoreboard
            assert(phys_reg->flatIndex() < _numOtherRegs);
            return otherScoreBoard[phys_reg->flatIndex()];
        }
    }

    /** Sets the register as ready. */
    void
    setReg(PhysRegIdPtr phys_reg)
    {
        if (phys_reg->isAlwaysReady()) {
            return;
        }

        switch (phys_reg->classValue()) {
          case IntRegClass:
            assert(phys_reg->index() < _numPregs);
            pregScoreBoard[phys_reg->index()] = true;
            break;
          case FloatRegClass:
            assert(phys_reg->index() < _numEregs);
            eregScoreBoard[phys_reg->index()] = true;
            break;
          case VecRegClass:
            assert(phys_reg->index() < _numVregs);
            vregScoreBoard[phys_reg->index()] = true;
            break;
          default:
            assert(phys_reg->flatIndex() < _numOtherRegs);
            otherScoreBoard[phys_reg->flatIndex()] = true;
            break;
        }

        DPRINTF(Scoreboard, "Setting reg %i (%s) as ready\n",
                phys_reg->index(), phys_reg->className());
    }

    /** Sets the register as not ready. */
    void
    unsetReg(PhysRegIdPtr phys_reg)
    {
        if (phys_reg->isAlwaysReady()) {
            return;
        }

        switch (phys_reg->classValue()) {
          case IntRegClass:
            assert(phys_reg->index() < _numPregs);
            pregScoreBoard[phys_reg->index()] = false;
            break;
          case FloatRegClass:
            assert(phys_reg->index() < _numEregs);
            eregScoreBoard[phys_reg->index()] = false;
            break;
          case VecRegClass:
            assert(phys_reg->index() < _numVregs);
            vregScoreBoard[phys_reg->index()] = false;
            break;
          default:
            assert(phys_reg->flatIndex() < _numOtherRegs);
            otherScoreBoard[phys_reg->flatIndex()] = false;
            break;
        }

        DPRINTF(Scoreboard, "Setting reg %i (%s) as busy\n",
                phys_reg->index(), phys_reg->className());
    }
};

} // namespace o3
} // namespace gem5

#endif
