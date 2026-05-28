/*
 * Copyright (c) 2010-2013, 2018-2019, 2025 Arm Limited
 * Copyright (c) 2013 Advanced Micro Devices, Inc.
 * All rights reserved.
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
 * Copyright (c) 2004-2006 The Regents of The University of Michigan
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

// @todo: Fix the instantaneous communication among all the stages within
// iew.  There's a clear delay between issue and execute, yet backwards
// communication happens simultaneously.

#include "cpu/o3/iew.hh"

#include <queue>

#include "cpu/checker/cpu.hh"
#include "cpu/o3/dyn_inst.hh"
#include "cpu/o3/fu_pool.hh"
#include "cpu/o3/inst_queue.hh"
#include "cpu/o3/limits.hh"
#include "cpu/timebuf.hh"
#include "debug/Activity.hh"
#include "debug/Drain.hh"
#include "debug/IEW.hh"
#include "params/BaseO3CPU.hh"

namespace gem5
{

namespace o3
{

// clang-format off
std::string IEW::IEWStats::statusStrings[ThreadStatusMax] = {
    "running",
    "blocked",
    "idle",
    "startSquash",
    "squashing",
    "unblocking",
};
// clang-format on

IEW::IEW(CPU *_cpu, const BaseO3CPUParams &params)
    : issueToExecQueue(params.backComSize, params.forwardComSize),
      cpu(_cpu),
      instQueue(_cpu, this, params),
      ldstQueue(_cpu, this, params),
      commitToIEWDelay(params.commitToIEWDelay),
      renameToIEWDelay(params.renameToIEWDelay),
      issueToExecuteDelay(params.issueToExecuteDelay),
      dispatchWidth(params.dispatchWidth),
      issueWidth(params.issueWidth),
      wbNumInst(0),
      wbCycle(0),
      wbWidth(params.wbWidth),
      pregWBWidth(params.pregWBWidth),
      vregWBWidth(params.vregWBWidth),
      eregWBWidth(params.eregWBWidth),
      numExecutingIntDiv(0),
      numThreads(params.numThreads),
      iewStats(cpu)
{
    if (dispatchWidth > MaxWidth)
        fatal("dispatchWidth (%d) is larger than compiled limit (%d),\n"
             "\tincrease MaxWidth in src/cpu/o3/limits.hh\n",
             dispatchWidth, static_cast<int>(MaxWidth));
    if (issueWidth > MaxWidth)
        fatal("issueWidth (%d) is larger than compiled limit (%d),\n"
             "\tincrease MaxWidth in src/cpu/o3/limits.hh\n",
             issueWidth, static_cast<int>(MaxWidth));
    if (wbWidth > MaxWidth)
        fatal("wbWidth (%d) is larger than compiled limit (%d),\n"
             "\tincrease MaxWidth in src/cpu/o3/limits.hh\n",
             wbWidth, static_cast<int>(MaxWidth));

    _status = Active;
    exeStatus = Running;
    wbStatus = Idle;

    // Setup wire to read instructions coming from issue.
    fromIssue = issueToExecQueue.getWire(-issueToExecuteDelay);

    // Instruction queue needs the queue between issue and execute.
    instQueue.setIssueToExecuteQueue(&issueToExecQueue);

    // Retrieve a list of all available FU pools
    fuPools = instQueue.allFUPools();

    for (ThreadID tid = 0; tid < MaxThreads; tid++) {
        dispatchStatus[tid] = Running;
        fetchRedirect[tid] = false;
    }

    updateLSQNextCycle = false;

    skidBufferMax = (renameToIEWDelay + 1) * params.renameWidth;
}

std::string
IEW::name() const
{
    return cpu->name() + ".iew";
}

void
IEW::regProbePoints()
{
    ppDispatch = new ProbePointArg<DynInstPtr>(
            cpu->getProbeManager(), "Dispatch");
    ppMispredict = new ProbePointArg<DynInstPtr>(
            cpu->getProbeManager(), "Mispredict");
    /**
     * Probe point with dynamic instruction as the argument used to probe when
     * an instruction starts to execute.
     */
    ppExecute = new ProbePointArg<DynInstPtr>(
            cpu->getProbeManager(), "Execute");
    /**
     * Probe point with dynamic instruction as the argument used to probe when
     * an instruction execution completes and it is marked ready to commit.
     */
    ppToCommit = new ProbePointArg<DynInstPtr>(
            cpu->getProbeManager(), "ToCommit");
}

IEW::IEWStats::IEWStats(CPU *cpu)
    : statistics::Group(cpu, "iew"),
      ADD_STAT(dispatchStatus, statistics::units::Cycle::get(),
               "Dispatch status cycles"),
      ADD_STAT(execStatus, statistics::units::Cycle::get(),
               "Number of cycles IEW is blocking"),
      ADD_STAT(dispatchedInsts, statistics::units::Count::get(),
               "Number of instructions dispatched to IQ"),
      ADD_STAT(dispSquashedInsts, statistics::units::Count::get(),
               "Number of squashed instructions skipped by dispatch"),
      ADD_STAT(dispLoadInsts, statistics::units::Count::get(),
               "Number of dispatched load instructions"),
      ADD_STAT(dispStoreInsts, statistics::units::Count::get(),
               "Number of dispatched store instructions"),
      ADD_STAT(dispNonSpecInsts, statistics::units::Count::get(),
               "Number of dispatched non-speculative instructions"),
      ADD_STAT(iqFullEvents, statistics::units::Count::get(),
               "Number of times the IQ has become full, causing a stall"),
      ADD_STAT(lsqFullEvents, statistics::units::Count::get(),
               "Number of times the LSQ has become full, causing a stall"),
      ADD_STAT(memOrderViolationEvents, statistics::units::Count::get(),
               "Number of memory order violations"),
      ADD_STAT(predictedTakenIncorrect, statistics::units::Count::get(),
               "Number of branches that were predicted taken incorrectly"),
      ADD_STAT(predictedNotTakenIncorrect, statistics::units::Count::get(),
               "Number of branches that were predicted not taken incorrectly"),
      ADD_STAT(branchMispredicts, statistics::units::Count::get(),
               "Number of branch mispredicts detected at execute",
               predictedTakenIncorrect + predictedNotTakenIncorrect),
      executedInstStats(cpu),
      ADD_STAT(instsToCommit, statistics::units::Count::get(),
               "Cumulative count of insts sent to commit"),
      ADD_STAT(writebackCount, statistics::units::Count::get(),
               "Cumulative count of insts written-back"),
      ADD_STAT(producerInst, statistics::units::Count::get(),
               "Number of instructions producing a value"),
      ADD_STAT(consumerInst, statistics::units::Count::get(),
               "Number of instructions consuming a value"),
      ADD_STAT(wbRate,
               statistics::units::Rate<statistics::units::Count,
                                       statistics::units::Cycle>::get(),
               "Insts written-back per cycle"),
      ADD_STAT(wbFanout,
               statistics::units::Rate<statistics::units::Count,
                                       statistics::units::Count>::get(),
               "Average fanout of values written-back"),
      ADD_STAT(stallCycles, statistics::units::Cycle::get(),
               "Cycles spent in each stall type (C910 classification)"),
      ADD_STAT(backendStallCycles, statistics::units::Cycle::get(),
               "Backend stall cycles (idu_hpcp_backend_stall)"),
      ADD_STAT(fenceSyncCount, statistics::units::Count::get(),
               "FENCE synchronization events"),
      ADD_STAT(pipeIssueCount, statistics::units::Count::get(),
               "Per-pipe issue count (C910 HPCP: idu_hpcp_rf_pipe[0:7]_inst_vld)"),
      ADD_STAT(issueLatchFail, statistics::units::Count::get(),
               "Per-pipe issue latch failure count"),
      ADD_STAT(iqEmptyCycles, statistics::units::Cycle::get(),
               "Cycles with IQ empty (idu_had_iq_empty)"),
      ADD_STAT(pipelineEmptyCycles, statistics::units::Cycle::get(),
               "Cycles with pipeline empty (idu_had_pipeline_empty)"),
      ADD_STAT(pipelineStallCycles, statistics::units::Cycle::get(),
               "Cycles with pipeline stall (idu_had_pipe_stall)")
{
    dispatchStatus.init(ThreadStatusMax)
        .flags(statistics::pdf | statistics::nozero);
    execStatus.init(ThreadStatusMax)
        .flags(statistics::pdf | statistics::nozero);
    for (int i = 0; i < ThreadStatusMax; ++i) {
        dispatchStatus.subname(i, statusStrings[i]);
        dispatchStatus.subdesc(i, "Number of cycles dispatch is " +
                                      statusStrings[i]);
        execStatus.subname(i, statusStrings[i]);
        execStatus.subdesc(i,
                           "Number of cycles dispatch is " + statusStrings[i]);
    }

    instsToCommit
        .init(cpu->numThreads)
        .flags(statistics::total);

    writebackCount
        .init(cpu->numThreads)
        .flags(statistics::total);

    producerInst
        .init(cpu->numThreads)
        .flags(statistics::total);

    consumerInst
        .init(cpu->numThreads)
        .flags(statistics::total);

    wbRate
        .flags(statistics::total);
    wbRate = writebackCount / cpu->baseStats.numCycles;

    wbFanout
        .flags(statistics::total);
    wbFanout = producerInst / consumerInst;

    // C910 stall type counters
    static const char* stallNames[] = {
        "no_stall", "rob_full", "iq_full", "vmb_full",
        "type_stall", "dispatch_stall", "is_stall",
        "div_stall", "vdiv_stall", "backend_stall"
    };
    stallCycles.init(NUM_STALL_TYPES)
        .flags(statistics::pdf | statistics::nozero);
    for (int i = 0; i < NUM_STALL_TYPES; ++i) {
        stallCycles.subname(i, stallNames[i]);
    }
    stallCycles.subdesc(0, "Cycles with no stall");
    stallCycles.subdesc(1, "Cycles stalled by ROB full");
    stallCycles.subdesc(2, "Cycles stalled by IQ full");
    stallCycles.subdesc(3, "Cycles stalled by VMB full");
    stallCycles.subdesc(4, "Cycles stalled by FENCE/barrier type stall");
    stallCycles.subdesc(5, "Cycles stalled by dispatch (composite)");
    stallCycles.subdesc(6, "Cycles stalled by IS-level total stall");
    stallCycles.subdesc(7, "Cycles stalled by DIV unit writeback");
    stallCycles.subdesc(8, "Cycles stalled by vector DIV writeback");
    stallCycles.subdesc(9, "Cycles stalled by backend execution units");

    // C910 HPCP pipe issue counters
    static const char* pipeNames[] = {
        "pipe0_iu_alu", "pipe1_iu_alu_mla", "pipe2_bju", "pipe3_lsu_load",
        "pipe4_lsu_special", "pipe5_lsu_store", "pipe6_vfpu_alu", "pipe7_vfpu_ma"
    };
    pipeIssueCount.init(8)
        .flags(statistics::total);
    for (int i = 0; i < 8; ++i) {
        pipeIssueCount.subname(i, pipeNames[i]);
    }
    issueLatchFail.init(8)
        .flags(statistics::total);
    for (int i = 0; i < 8; ++i) {
        issueLatchFail.subname(i, pipeNames[i]);
    }
}

IEW::IEWStats::ExecutedInstStats::ExecutedInstStats(CPU *cpu)
    : statistics::Group(cpu),
    ADD_STAT(numSquashedInsts, statistics::units::Count::get(),
             "Number of squashed instructions skipped in execute"),
    ADD_STAT(numSwp, statistics::units::Count::get(),
             "Number of swp insts executed")
{
    numSwp
        .init(cpu->numThreads)
        .flags(statistics::total);
}

void
IEW::startupStage()
{
    for (ThreadID tid = 0; tid < numThreads; tid++) {
        toRename->iewInfo[tid].usedIQ = true;
        toRename->iewInfo[tid].freeIQEntries =
            instQueue.numFreeEntries(tid);

        toRename->iewInfo[tid].usedLSQ = true;
        toRename->iewInfo[tid].freeLQEntries =
            ldstQueue.numFreeLoadEntries(tid);
        toRename->iewInfo[tid].freeSQEntries =
            ldstQueue.numFreeStoreEntries(tid);
    }

    // Initialize the checker's dcache port here
    if (cpu->checker) {
        cpu->checker->setDcachePort(&ldstQueue.getDataPort());
    }

    cpu->activateStage(CPU::IEWIdx);
}

void
IEW::clearStates(ThreadID tid)
{
    toRename->iewInfo[tid].usedIQ = true;
    toRename->iewInfo[tid].freeIQEntries =
        instQueue.numFreeEntries(tid);

    toRename->iewInfo[tid].usedLSQ = true;
    toRename->iewInfo[tid].freeLQEntries = ldstQueue.numFreeLoadEntries(tid);
    toRename->iewInfo[tid].freeSQEntries = ldstQueue.numFreeStoreEntries(tid);

    // Clear out any of this thread's instructions being sent to commit.
    for (int i = -cpu->iewQueue.getPast();
         i <= cpu->iewQueue.getFuture(); ++i) {
        IEWStruct& iew_struct = cpu->iewQueue[i];
        removeCommThreadInsts(tid, iew_struct);
        iew_struct.mispredictInst[tid] = nullptr;
        iew_struct.mispredPC[tid] = 0;
        iew_struct.squashedSeqNum[tid] = 0;
        iew_struct.pc[tid] = nullptr;
        iew_struct.squash[tid] = false;
        iew_struct.branchMispredict[tid] = false;
        iew_struct.branchTaken[tid] = false;
        iew_struct.includeSquashInst[tid] = false;
    }

    // Clear out any of this thread's instructions being sent from
    // issue to execute.
    for (int i = -issueToExecQueue.getPast();
         i <= issueToExecQueue.getFuture(); ++i)
        removeCommThreadInsts(tid, issueToExecQueue[i]);

    // Clear out any of this thread's instructions being sent to prior stages.
    for (int i = -cpu->timeBuffer.getPast();
         i <= cpu->timeBuffer.getFuture(); ++i) {
        TimeStruct& time_struct = cpu->timeBuffer[i];
        time_struct.iewInfo[tid] = {};
        time_struct.iewBlock[tid] = false;
        time_struct.iewUnblock[tid] = false;
    }
}

void
IEW::setTimeBuffer(TimeBuffer<TimeStruct> *tb_ptr)
{
    timeBuffer = tb_ptr;

    // Setup wire to read information from time buffer, from commit.
    fromCommit = timeBuffer->getWire(-commitToIEWDelay);

    // Setup wire to write information back to previous stages.
    toRename = timeBuffer->getWire(0);

    toFetch = timeBuffer->getWire(0);

    // Instruction queue also needs main time buffer.
    instQueue.setTimeBuffer(tb_ptr);
}

void
IEW::setRenameQueue(TimeBuffer<RenameStruct> *rq_ptr)
{
    renameQueue = rq_ptr;

    // Setup wire to read information from rename queue.
    fromRename = renameQueue->getWire(-renameToIEWDelay);
}

void
IEW::setIEWQueue(TimeBuffer<IEWStruct> *iq_ptr)
{
    iewQueue = iq_ptr;

    // Setup wire to write instructions to commit.
    toCommit = iewQueue->getWire(0);
}

void
IEW::setActiveThreads(std::list<ThreadID> *at_ptr)
{
    activeThreads = at_ptr;

    ldstQueue.setActiveThreads(at_ptr);
    instQueue.setActiveThreads(at_ptr);
}

void
IEW::setScoreboard(Scoreboard *sb_ptr)
{
    scoreboard = sb_ptr;
}

bool
IEW::isDrained() const
{
    bool drained = ldstQueue.isDrained() && instQueue.isDrained();

    for (ThreadID tid = 0; tid < numThreads; tid++) {
        if (!insts[tid].empty()) {
            DPRINTF(Drain, "%i: Insts not empty.\n", tid);
            drained = false;
        }
        if (!skidBuffer[tid].empty()) {
            DPRINTF(Drain, "%i: Skid buffer not empty.\n", tid);
            drained = false;
        }
        drained = drained && dispatchStatus[tid] == Running;
    }

    // Also check the FU pool as instructions are "stored" in FU
    // completion events until they are done and not accounted for
    // above
    for (FUPool *fu_pool : fuPools) {
        if (!fu_pool->isDrained()) {
            DPRINTF(Drain, "FU pool still busy.\n");
            drained = false;
            break;
        }
    }

    return drained;
}

void
IEW::drainSanityCheck() const
{
    assert(isDrained());

    instQueue.drainSanityCheck();
    ldstQueue.drainSanityCheck();
}

void
IEW::takeOverFrom()
{
    // Reset all state.
    _status = Active;
    exeStatus = Running;
    wbStatus = Idle;

    instQueue.takeOverFrom();
    ldstQueue.takeOverFrom();
    for (FUPool *fu_pool : fuPools) {
        fu_pool->takeOverFrom();
    }

    startupStage();
    cpu->activityThisCycle();

    for (ThreadID tid = 0; tid < numThreads; tid++) {
        dispatchStatus[tid] = Running;
        fetchRedirect[tid] = false;
    }

    updateLSQNextCycle = false;

    for (int i = 0; i < issueToExecQueue.getSize(); ++i) {
        issueToExecQueue.advance();
    }
}

void
IEW::squash(ThreadID tid)
{
    DPRINTF(IEW, "[tid:%i] Squashing all instructions.\n", tid);

    // C910: Log flush type for debugging
    FlushType ft = fromCommit->commitInfo[tid].flushType;
    DPRINTF(IEW, "[tid:%i] Flush type: %s\n", tid, to_string(ft));

    // Tell the IQ to start squashing.
    instQueue.squash(tid);

    // Tell the LDSTQ to start squashing.
    ldstQueue.squash(fromCommit->commitInfo[tid].doneSeqNum, tid);
    updatedQueues = true;

    // Clear the skid buffer in case it has any data in it.
    DPRINTF(IEW,
            "Removing skidbuffer instructions until "
            "[sn:%llu] [tid:%i]\n",
            fromCommit->commitInfo[tid].doneSeqNum, tid);

    while (!skidBuffer[tid].empty()) {
        if (skidBuffer[tid].front()->isLoad()) {
            toRename->iewInfo[tid].dispatchedToLQ++;
        }
        if (skidBuffer[tid].front()->isStore() ||
            skidBuffer[tid].front()->isAtomic()) {
            toRename->iewInfo[tid].dispatchedToSQ++;
        }

        toRename->iewInfo[tid].dispatched++;

        skidBuffer[tid].pop();
    }

    emptyRenameInsts(tid);
}

void
IEW::squashDueToBranch(const DynInstPtr& inst, ThreadID tid)
{
    DPRINTF(IEW, "[tid:%i] [sn:%llu] Squashing from a specific instruction,"
            " PC: %s "
            "\n", tid, inst->seqNum, inst->pcState() );

    if (!toCommit->squash[tid] ||
            inst->seqNum < toCommit->squashedSeqNum[tid]) {
        toCommit->squash[tid] = true;
        toCommit->squashedSeqNum[tid] = inst->seqNum;
        toCommit->branchTaken[tid] = inst->pcState().branching();

        set(toCommit->pc[tid], inst->pcState());
        inst->staticInst->advancePC(*toCommit->pc[tid]);

        toCommit->mispredictInst[tid] = inst;
        toCommit->includeSquashInst[tid] = false;

        wroteToTimeBuffer = true;
    }

}

void
IEW::squashDueToMemOrder(const DynInstPtr& inst, ThreadID tid)
{
    DPRINTF(IEW, "[tid:%i] Memory violation, squashing violator and younger "
            "insts, PC: %s [sn:%llu].\n", tid, inst->pcState(), inst->seqNum);
    // Need to include inst->seqNum in the following comparison to cover the
    // corner case when a branch misprediction and a memory violation for the
    // same instruction (e.g. load PC) are detected in the same cycle.  In this
    // case the memory violator should take precedence over the branch
    // misprediction because it requires the violator itself to be included in
    // the squash.
    if (!toCommit->squash[tid] ||
            inst->seqNum <= toCommit->squashedSeqNum[tid]) {
        toCommit->squash[tid] = true;

        toCommit->squashedSeqNum[tid] = inst->seqNum;
        set(toCommit->pc[tid], inst->pcState());
        toCommit->mispredictInst[tid] = NULL;

        // Must include the memory violator in the squash.
        toCommit->includeSquashInst[tid] = true;

        wroteToTimeBuffer = true;
    }
}

void
IEW::block(ThreadID tid)
{
    DPRINTF(IEW, "[tid:%i] Blocking.\n", tid);

    if (dispatchStatus[tid] != Blocked &&
        dispatchStatus[tid] != Unblocking) {
        toRename->iewBlock[tid] = true;
        wroteToTimeBuffer = true;
    }

    // Add the current inputs to the skid buffer so they can be
    // reprocessed when this stage unblocks.
    skidInsert(tid);

    dispatchStatus[tid] = Blocked;
}

void
IEW::unblock(ThreadID tid)
{
    DPRINTF(IEW, "[tid:%i] Reading instructions out of the skid "
            "buffer %u.\n",tid, tid);

    // If the skid bufffer is empty, signal back to previous stages to unblock.
    // Also switch status to running.
    if (skidBuffer[tid].empty()) {
        toRename->iewUnblock[tid] = true;
        wroteToTimeBuffer = true;
        DPRINTF(IEW, "[tid:%i] Done unblocking.\n",tid);
        dispatchStatus[tid] = Running;
    }
}

void
IEW::wakeDependents(const DynInstPtr& inst)
{
    instQueue.wakeDependents(inst);
}

void
IEW::rescheduleMemInst(const DynInstPtr& inst)
{
    instQueue.rescheduleMemInst(inst);
}

void
IEW::replayMemInst(const DynInstPtr& inst)
{
    instQueue.replayMemInst(inst);
}

void
IEW::blockMemInst(const DynInstPtr& inst)
{
    instQueue.blockMemInst(inst);
}

void
IEW::retryMemInst(const DynInstPtr& inst)
{
    instQueue.retryMemInst(inst);
}

void
IEW::cacheUnblocked()
{
    instQueue.cacheUnblocked();
}

void
IEW::instToCommit(const DynInstPtr& inst)
{
    // This function should not be called after writebackInsts in a
    // single cycle.  That will cause problems with an instruction
    // being added to the queue to commit without being processed by
    // writebackInsts prior to being sent to commit.

    // First check the time slot that this instruction will write
    // to.  If there are free write ports at the time, then go ahead
    // and write the instruction to that time.  If there are not,
    // keep looking back to see where's the first time there's a
    // free slot.
    while ((*iewQueue)[wbCycle].insts[wbNumInst]) {
        ++wbNumInst;
        if (wbNumInst == wbWidth) {
            ++wbCycle;
            wbNumInst = 0;
        }
    }

    DPRINTF(IEW, "Current wb cycle: %i, width: %i, numInst: %i\nwbActual:%i\n",
            wbCycle, wbWidth, wbNumInst, wbCycle * wbWidth + wbNumInst);
    // Add finished instruction to queue to commit.
    (*iewQueue)[wbCycle].insts[wbNumInst] = inst;
    (*iewQueue)[wbCycle].size++;
}

void
IEW::skidInsert(ThreadID tid)
{
    DynInstPtr inst = NULL;

    while (!insts[tid].empty()) {
        inst = insts[tid].front();

        insts[tid].pop();

        DPRINTF(IEW,"[tid:%i] Inserting [sn:%lli] PC:%s into "
                "dispatch skidBuffer %i\n",tid, inst->seqNum,
                inst->pcState(),tid);

        skidBuffer[tid].push(inst);
    }

    assert(skidBuffer[tid].size() <= skidBufferMax &&
           "Skidbuffer Exceeded Max Size");
}

int
IEW::skidCount()
{
    int max=0;

    for (ThreadID tid : *activeThreads) {
        unsigned thread_count = skidBuffer[tid].size();
        if (max < thread_count)
            max = thread_count;
    }

    return max;
}

bool
IEW::skidsEmpty()
{
    for (ThreadID tid : *activeThreads) {
        if (!skidBuffer[tid].empty())
            return false;
    }

    return true;
}

void
IEW::updateStatus()
{
    bool any_unblocking = false;

    for (ThreadID tid : *activeThreads) {
        if (dispatchStatus[tid] == Unblocking) {
            any_unblocking = true;
            break;
        }
    }

    // If there are no ready instructions waiting to be scheduled by the IQ,
    // and there's no stores waiting to write back, and dispatch is not
    // unblocking, then there is no internal activity for the IEW stage.
    instQueue.iqIOStats.intInstQueueReads++;
    if (_status == Active && !instQueue.hasReadyInsts() &&
        !ldstQueue.willWB() && !any_unblocking) {
        DPRINTF(IEW, "IEW switching to idle\n");

        deactivateStage();

        _status = Inactive;
    } else if (_status == Inactive && (instQueue.hasReadyInsts() ||
                                       ldstQueue.willWB() ||
                                       any_unblocking)) {
        // Otherwise there is internal activity.  Set to active.
        DPRINTF(IEW, "IEW switching to active\n");

        activateStage();

        _status = Active;
    }
}

bool
IEW::checkStall(ThreadID tid)
{
    bool ret_val(false);

    if (fromCommit->commitInfo[tid].robSquashing) {
        DPRINTF(IEW,"[tid:%i] Stall from Commit stage detected.\n",tid);
        ret_val = true;
    } else if (instQueue.isFull(tid)) {
        DPRINTF(IEW,"[tid:%i] Stall: IQ  is full.\n",tid);
        ret_val = true;
    }

    return ret_val;
}

IEW::StallType
IEW::detectStallType(ThreadID tid)
{
    // C910 stall type detection: checks in priority order
    // ctrl_is_dis_stall = ctrl_is_rob_full || ctrl_is_iq_full || ctrl_is_vmb_full
    // ctrl_is_stall = ctrl_is_dis_stall || is_dis_type_stall

    if (fromCommit->commitInfo[tid].robSquashing) {
        return ROB_FULL;
    }

    if (isROBFull(tid)) {
        return ROB_FULL;
    }

    if (isIQFull(tid)) {
        return IQ_FULL;
    }

    if (isVMBFull(tid)) {
        return VMB_FULL;
    }

    if (isTypeStall(tid)) {
        return TYPE_STALL;
    }

    if (isDivStall(tid)) {
        return DIV_STALL;
    }

    return NO_STALL;
}

bool
IEW::isROBFull(ThreadID tid)
{
    return fromCommit->commitInfo[tid].robSquashing ||
           fromCommit->commitInfo[tid].freeROBEntries == 0;
}

bool
IEW::isIQFull(ThreadID tid)
{
    return instQueue.isFull(tid);
}

bool
IEW::isIQFullByType(ThreadID tid, IQType type)
{
    return instQueue.isFullByType(type, tid);
}

bool
IEW::isVMBFull(ThreadID tid)
{
    return instQueue.isFullByType(IQType::VMB, tid);
}

bool
IEW::isTypeStall(ThreadID tid)
{
    // Check if there's a barrier/FENCE blocking subsequent instructions
    // This is detected by checking if any barrier instruction is in the IQ
    // and hasn't completed yet.
    return false; // Placeholder — would need IQ barrier tracking
}

bool
IEW::isDispatchStall(ThreadID tid)
{
    // ctrl_is_dis_stall = ctrl_is_rob_full || ctrl_is_iq_full || ctrl_is_vmb_full
    return isROBFull(tid) || isIQFull(tid) || isVMBFull(tid);
}

bool
IEW::isDivStall(ThreadID tid)
{
    // DIV_STALL: IntDiv FU busy (non-pipelined, 20 cycles) with pending DIV instructions
    // Check if there are ready IntDiv instructions in the IQ waiting for FU
    if (!instQueue.hasReadyIntDiv()) {
        return false;
    }
    // If IntDiv FUs are occupied and there are more DIVs waiting, it's a DIV stall
    return numExecutingIntDiv > 0;
}

void
IEW::checkSignalsAndUpdate(ThreadID tid)
{
    // Check if there's a squash signal, squash if there is
    // Check stall signals, block if there is.
    // If status was Blocked
    //     if so then go to unblocking
    // If status was Squashing
    //     check if squashing is not high.  Switch to running this cycle.

    if (fromCommit->commitInfo[tid].squash) {
        squash(tid);

        if (dispatchStatus[tid] == Blocked ||
            dispatchStatus[tid] == Unblocking) {
            toRename->iewUnblock[tid] = true;
            wroteToTimeBuffer = true;
        }

        dispatchStatus[tid] = Squashing;
        fetchRedirect[tid] = false;
        return;
    }

    if (fromCommit->commitInfo[tid].robSquashing) {
        DPRINTF(IEW, "[tid:%i] ROB is still squashing.\n", tid);

        dispatchStatus[tid] = Squashing;
        emptyRenameInsts(tid);
        wroteToTimeBuffer = true;
    }

    if (checkStall(tid)) {
        block(tid);
        dispatchStatus[tid] = Blocked;
        return;
    }

    if (dispatchStatus[tid] == Blocked) {
        // Status from previous cycle was blocked, but there are no more stall
        // conditions.  Switch over to unblocking.
        DPRINTF(IEW, "[tid:%i] Done blocking, switching to unblocking.\n",
                tid);

        dispatchStatus[tid] = Unblocking;

        unblock(tid);

        return;
    }

    if (dispatchStatus[tid] == Squashing) {
        // Switch status to running if rename isn't being told to block or
        // squash this cycle.
        DPRINTF(IEW, "[tid:%i] Done squashing, switching to running.\n",
                tid);

        dispatchStatus[tid] = Running;

        return;
    }
}

void
IEW::sortInsts()
{
    int insts_from_rename = fromRename->size;
#ifdef GEM5_DEBUG
    for (ThreadID tid = 0; tid < numThreads; tid++)
        assert(insts[tid].empty());
#endif
    for (int i = 0; i < insts_from_rename; ++i) {
        insts[fromRename->insts[i]->threadNumber].push(fromRename->insts[i]);
    }
}

void
IEW::emptyRenameInsts(ThreadID tid)
{
    DPRINTF(IEW, "[tid:%i] Removing incoming rename instructions\n", tid);

    while (!insts[tid].empty()) {

        if (insts[tid].front()->isLoad()) {
            toRename->iewInfo[tid].dispatchedToLQ++;
        }
        if (insts[tid].front()->isStore() ||
            insts[tid].front()->isAtomic()) {
            toRename->iewInfo[tid].dispatchedToSQ++;
        }

        toRename->iewInfo[tid].dispatched++;

        insts[tid].pop();
    }
}

void
IEW::wakeCPU()
{
    cpu->wakeCPU();
}

void
IEW::activityThisCycle()
{
    DPRINTF(Activity, "Activity this cycle.\n");
    cpu->activityThisCycle();
}

void
IEW::activateStage()
{
    DPRINTF(Activity, "Activating stage.\n");
    cpu->activateStage(CPU::IEWIdx);
}

void
IEW::deactivateStage()
{
    DPRINTF(Activity, "Deactivating stage.\n");
    cpu->deactivateStage(CPU::IEWIdx);
}

void
IEW::dispatch(ThreadID tid)
{
    // If status is Running or idle,
    //     call dispatchInsts()
    // If status is Unblocking,
    //     buffer any instructions coming from rename
    //     continue trying to empty skid buffer
    //     check if stall conditions have passed
    iewStats.dispatchStatus[dispatchStatus[tid]]++;

    // Dispatch should try to dispatch as many instructions as its bandwidth
    // will allow, as long as it is not currently blocked.
    if (dispatchStatus[tid] == Running ||
        dispatchStatus[tid] == Idle) {
        DPRINTF(IEW, "[tid:%i] Not blocked, so attempting to run "
                "dispatch.\n", tid);

        dispatchInsts(tid);
    } else if (dispatchStatus[tid] == Unblocking) {
        // Make sure that the skid buffer has something in it if the
        // status is unblocking.
        assert(!skidsEmpty());

        // If the status was unblocking, then instructions from the skid
        // buffer were used.  Remove those instructions and handle
        // the rest of unblocking.
        dispatchInsts(tid);

        if (fromRename->size != 0) {
            // Add the current inputs to the skid buffer so they can be
            // reprocessed when this stage unblocks.
            skidInsert(tid);
        }

        unblock(tid);
    }
}

void
IEW::dispatchInsts(ThreadID tid)
{
    // Obtain instructions from skid buffer if unblocking, or queue from rename
    // otherwise.
    std::queue<DynInstPtr> &insts_to_dispatch =
        dispatchStatus[tid] == Unblocking ?
        skidBuffer[tid] : insts[tid];

    int insts_to_add = insts_to_dispatch.size();

    DynInstPtr inst;
    IQType targetIQ = IQType::AIQ0;
    bool add_to_iq = false;
    int dis_num_inst = 0;

    // Loop through the instructions, putting them in the instruction
    // queue.
    for ( ; dis_num_inst < insts_to_add &&
              dis_num_inst < dispatchWidth;
          ++dis_num_inst)
    {
        inst = insts_to_dispatch.front();

        if (dispatchStatus[tid] == Unblocking) {
            DPRINTF(IEW, "[tid:%i] Issue: Examining instruction from skid "
                    "buffer\n", tid);
        }

        // Make sure there's a valid instruction there.
        assert(inst);

        DPRINTF(IEW, "[tid:%i] Issue: Adding PC %s [sn:%lli] [tid:%i] to "
                "IQ.\n",
                tid, inst->pcState(), inst->seqNum, inst->threadNumber);

        // Be sure to mark these instructions as ready so that the
        // commit stage can go ahead and execute them, and mark
        // them as issued so the IQ doesn't reprocess them.

        // Check for squashed instructions.
        if (inst->isSquashed()) {
            DPRINTF(IEW, "[tid:%i] Issue: Squashed instruction encountered, "
                    "not adding to IQ.\n", tid);

            ++iewStats.dispSquashedInsts;

            insts_to_dispatch.pop();

            //Tell Rename That An Instruction has been processed
            if (inst->isLoad()) {
                toRename->iewInfo[tid].dispatchedToLQ++;
            }
            if (inst->isStore() || inst->isAtomic()) {
                toRename->iewInfo[tid].dispatchedToSQ++;
            }

            toRename->iewInfo[tid].dispatched++;

            continue;
        }

        // Check for full conditions.
        if (instQueue.isFull(inst)) {
            DPRINTF(IEW, "[tid:%i] Issue: IQ has become full.\n", tid);

            // Call function to start blocking.
            block(tid);

            // Set unblock to false. Special case where we are using
            // skidbuffer (unblocking) instructions but then we still
            // get full in the IQ.
            toRename->iewUnblock[tid] = false;

            ++iewStats.iqFullEvents;
            break;
        }

        // Check LSQ if inst is LD/ST
        if ((inst->isAtomic() && ldstQueue.sqFull(tid)) ||
            (inst->isLoad() && ldstQueue.lqFull(tid)) ||
            (inst->isStore() && ldstQueue.sqFull(tid))) {
            DPRINTF(IEW, "[tid:%i] Issue: %s has become full.\n",tid,
                    inst->isLoad() ? "LQ" : "SQ");

            // Call function to start blocking.
            block(tid);

            // Set unblock to false. Special case where we are using
            // skidbuffer (unblocking) instructions but then we still
            // get full in the IQ.
            toRename->iewUnblock[tid] = false;

            ++iewStats.lsqFullEvents;
            break;
        }

        // hardware transactional memory
        // CPU needs to track transactional state in program order.
        const int numHtmStarts = ldstQueue.numHtmStarts(tid);
        const int numHtmStops = ldstQueue.numHtmStops(tid);
        const int htmDepth = numHtmStarts - numHtmStops;

        if (htmDepth > 0) {
            inst->setHtmTransactionalState(ldstQueue.getLatestHtmUid(tid),
                                            htmDepth);
        } else {
            inst->clearHtmTransactionalState();
        }


        // Determine the target IQ type based on instruction characteristics
        // (C910-style typed IQ routing)
        add_to_iq = false;

        // Otherwise issue the instruction just fine.
        if (inst->isAtomic()) {
            DPRINTF(IEW, "[tid:%i] Issue: Memory instruction "
                    "encountered, adding to LSQ.\n", tid);

            ldstQueue.insertStore(inst);

            ++iewStats.dispStoreInsts;

            // AMOs need to be set as "canCommit()"
            // so that commit can process them when they reach the
            // head of commit.
            inst->setCanCommit();
            instQueue.insertNonSpec(inst);

            ++iewStats.dispNonSpecInsts;

            toRename->iewInfo[tid].dispatchedToSQ++;
        } else if (inst->isLoad()) {
            DPRINTF(IEW, "[tid:%i] Issue: Memory instruction "
                    "encountered, adding to LSQ.\n", tid);

            // Reserve a spot in the load store queue for this
            // memory access.
            ldstQueue.insertLoad(inst);

            ++iewStats.dispLoadInsts;

            // Loads go to LSIQ for address calculation
            targetIQ = IQType::LSIQ;
            add_to_iq = true;

            toRename->iewInfo[tid].dispatchedToLQ++;
        } else if (inst->isStore()) {
            DPRINTF(IEW, "[tid:%i] Issue: Memory instruction "
                    "encountered, adding to LSQ.\n", tid);

            ldstQueue.insertStore(inst);

            ++iewStats.dispStoreInsts;

            if (inst->isStoreConditional()) {
                // Store conditionals need to be set as "canCommit()"
                // so that commit can process them when they reach the
                // head of commit.
                // @todo: This is somewhat specific to Alpha.
                inst->setCanCommit();
                instQueue.insertNonSpec(inst);

                ++iewStats.dispNonSpecInsts;
            } else {
                // Store address calculation → LSIQ (Pipe3)
                // Store data write → SDIQ (Pipe5)
                // In gem5, a single store instruction does both, so we
                // route to LSIQ for address calc. The LSQ handles data.
                targetIQ = IQType::LSIQ;
                add_to_iq = true;
            }

            toRename->iewInfo[tid].dispatchedToSQ++;
        } else if (inst->isReadBarrier() || inst->isWriteBarrier()) {
            // FENCE/barrier: original path — insert as barrier for
            // memory ordering, IQ entry assigned by findIQ.
            inst->setCanCommit();
            instQueue.insertBarrier(inst);
            add_to_iq = false;
        } else if (inst->isNop()) {
            DPRINTF(IEW, "[tid:%i] Issue: Nop instruction encountered, "
                    "skipping.\n", tid);

            inst->setIssued();
            inst->setExecuted();
            inst->setCanCommit();

            instQueue.recordProducer(inst);

            cpu->executeStats[tid]->numNop++;

            add_to_iq = false;
        } else {
            assert(!inst->isExecuted());
            // Route non-memory instructions to typed IQs
            add_to_iq = true;

            enums::OpClass opClass = inst->opClass();
            bool isBranch = inst->isControl();

            if (isBranch) {
                // Branches/jumps go to BIQ
                targetIQ = IQType::BIQ;

                // Mark procedure calls (CALL/JAL with link) and returns
                if (inst->isCall()) {
                    inst->markProcedureCall();
                }
                if (inst->isReturn()) {
                    inst->markReturnInst();
                }
            } else if (inst->isVector()) {
                // Vector instructions
                // Vector memory operations → VMB
                if (opClass == enums::SimdUnitStrideLoad ||
                    opClass == enums::SimdUnitStrideStore ||
                    opClass == enums::SimdUnitStrideMaskLoad ||
                    opClass == enums::SimdUnitStrideMaskStore ||
                    opClass == enums::SimdStridedLoad ||
                    opClass == enums::SimdStridedStore ||
                    opClass == enums::SimdIndexedLoad ||
                    opClass == enums::SimdIndexedStore ||
                    opClass == enums::SimdWholeRegisterLoad ||
                    opClass == enums::SimdWholeRegisterStore ||
                    opClass == enums::SimdUnitStrideFaultOnlyFirstLoad) {
                    targetIQ = IQType::VMB;
                } else if (opClass == enums::SimdFloatDiv ||
                           opClass == enums::SimdFloatSqrt ||
                           opClass == enums::SimdFloatMult ||
                           opClass == enums::SimdFloatMultAcc ||
                           opClass == enums::SimdFloatMatMultAcc) {
                    // Vector multiply/div → VIQ1 (VFMAU)
                    targetIQ = IQType::VIQ1;
                } else {
                    // Vector ALU → VIQ0
                    targetIQ = IQType::VIQ0;
                }
            } else if (inst->isFloating()) {
                // Scalar FP → VIQ1
                targetIQ = IQType::VIQ1;
            } else {
                // Integer ALU operations
                if (opClass == enums::IntMult ||
                    opClass == enums::FloatMult ||
                    opClass == enums::FloatMultAcc) {
                    // MUL/MADD → AIQ1 (has MLA unit)
                    targetIQ = IQType::AIQ1;
                } else if (opClass == enums::IntDiv ||
                           opClass == enums::FloatDiv ||
                           opClass == enums::FloatSqrt) {
                    // DIV/REM → AIQ1 (or AIQ0 if AIQ1 full)
                    targetIQ = IQType::AIQ1;
                } else {
                    // Integer ALU → round-robin AIQ0/AIQ1
                    targetIQ = (instQueue.getAIQRRCounter() % 2 == 0)
                        ? IQType::AIQ0 : IQType::AIQ1;
                    instQueue.advanceAIQRRCounter();
                }
            }
        }

        if (add_to_iq && inst->isNonSpeculative()) {
            DPRINTF(IEW, "[tid:%i] Issue: Nonspeculative instruction "
                    "encountered, skipping.\n", tid);

            // Same as non-speculative stores.
            inst->setCanCommit();

            // Specifically insert it as nonspeculative.
            instQueue.insertNonSpec(inst);

            ++iewStats.dispNonSpecInsts;

            add_to_iq = false;
        }

        // If the instruction queue is not full, then add the
        // instruction to the appropriate typed IQ.
        if (add_to_iq) {
            if (!instQueue.insertToIQType(inst, targetIQ)) {
                // Target IQ is full — try fallback routing.
                IQType fallbackIQ = IQType::NUM_IQ_TYPES;
                bool fallbackOk = false;

                switch (static_cast<int>(targetIQ)) {
                  case static_cast<int>(IQType::AIQ0):
                    fallbackIQ = IQType::AIQ1;
                    break;
                  case static_cast<int>(IQType::AIQ1):
                    fallbackIQ = IQType::AIQ0;
                    break;
                  case static_cast<int>(IQType::BIQ):
                    fallbackIQ = IQType::AIQ0;
                    break;
                  case static_cast<int>(IQType::VIQ0):
                    fallbackIQ = IQType::VIQ1;
                    break;
                  case static_cast<int>(IQType::VIQ1):
                    fallbackIQ = IQType::AIQ0;
                    break;
                  case static_cast<int>(IQType::VMB):
                    fallbackIQ = IQType::LSIQ;
                    break;
                  default:
                    break;
                }

                if (fallbackIQ != IQType::NUM_IQ_TYPES) {
                    fallbackOk = instQueue.insertToIQType(inst, fallbackIQ);
                }

                if (!fallbackOk && !instQueue.isFull(inst)) {
                    instQueue.insert(inst);
                    fallbackOk = true;
                }

                if (!fallbackOk) {
                    DPRINTF(IEW, "[tid:%i] Issue: IQ type %s is full.\n",
                            tid, to_string(targetIQ));
                    block(tid);
                    toRename->iewUnblock[tid] = false;
                    ++iewStats.iqFullEvents;
                    break;
                }
            }
        }

        insts_to_dispatch.pop();

        toRename->iewInfo[tid].dispatched++;

        ++iewStats.dispatchedInsts;

        inst->dispatchTick = curTick() - inst->fetchTick;

        ppDispatch->notify(inst);
    }

    if (!insts_to_dispatch.empty()) {
        DPRINTF(IEW,"[tid:%i] Issue: Bandwidth Full. Blocking.\n", tid);
        block(tid);
        toRename->iewUnblock[tid] = false;
    }

    if (dispatchStatus[tid] == Idle && dis_num_inst) {
        dispatchStatus[tid] = Running;

        updatedQueues = true;
    }

    dis_num_inst = 0;
}

void
IEW::printAvailableInsts()
{
    int inst = 0;

    std::cout << "Available Instructions: ";

    while (fromIssue->insts[inst]) {

        if (inst%3==0) std::cout << "\n\t";

        std::cout << "PC: " << fromIssue->insts[inst]->pcState()
             << " TN: " << fromIssue->insts[inst]->threadNumber
             << " SN: " << fromIssue->insts[inst]->seqNum << " | ";

        inst++;

    }

    std::cout << "\n";
}

void
IEW::executeInsts()
{
    wbNumInst = 0;
    wbCycle = 0;

    for (ThreadID tid : *activeThreads) {
        fetchRedirect[tid] = false;
    }

    // Uncomment this if you want to see all available instructions.
    // @todo This doesn't actually work anymore, we should fix it.
//    printAvailableInsts();

    // Execute/writeback any instructions that are available.
    int insts_to_execute = fromIssue->size;
    int inst_num = 0;
    for (; inst_num < insts_to_execute;
          ++inst_num) {

        DPRINTF(IEW, "Execute: Executing instructions from IQ.\n");

        DynInstPtr inst = instQueue.getInstToExecute();

        DPRINTF(IEW, "Execute: Processing PC %s, [tid:%i] [sn:%llu].\n",
                inst->pcState(), inst->threadNumber,inst->seqNum);

        // Notify potential listeners that this instruction has started
        // executing
        ppExecute->notify(inst);

        // Check if the instruction is squashed; if so then skip it
        if (inst->isSquashed()) {
            DPRINTF(IEW, "Execute: Instruction was squashed. PC: %s, [tid:%i]"
                         " [sn:%llu]\n", inst->pcState(), inst->threadNumber,
                         inst->seqNum);

            // Consider this instruction executed so that commit can go
            // ahead and retire the instruction.
            inst->setExecuted();

            // Not sure if I should set this here or just let commit try to
            // commit any squashed instructions.  I like the latter a bit more.
            inst->setCanCommit();

            ++iewStats.executedInstStats.numSquashedInsts;

            continue;
        }

        Fault fault = NoFault;

        // Execute instruction.
        // Note that if the instruction faults, it will be handled
        // at the commit stage.
        if (inst->isMemRef()) {
            DPRINTF(IEW, "Execute: Calculating address for memory "
                    "reference.\n");

            // Tell the LDSTQ to execute this instruction (if it is a load).
            if (inst->isAtomic()) {
                // AMOs are treated like store requests
                fault = ldstQueue.executeStore(inst);

                if (inst->isTranslationDelayed() &&
                    fault == NoFault) {
                    // A hw page table walk is currently going on; the
                    // instruction must be deferred.
                    DPRINTF(IEW, "Execute: Delayed translation, deferring "
                            "store.\n");
                    instQueue.deferMemInst(inst);
                    continue;
                }
            } else if (inst->isLoad()) {
                // Loads will mark themselves as executed, and their writeback
                // event adds the instruction to the queue to commit
                fault = ldstQueue.executeLoad(inst);

                if (inst->isTranslationDelayed() &&
                    fault == NoFault) {
                    // A hw page table walk is currently going on; the
                    // instruction must be deferred.
                    DPRINTF(IEW, "Execute: Delayed translation, deferring "
                            "load.\n");
                    instQueue.deferMemInst(inst);
                    continue;
                }

                if (inst->isDataPrefetch() || inst->isInstPrefetch()) {
                    inst->fault = NoFault;
                }
            } else if (inst->isStore()) {
                fault = ldstQueue.executeStore(inst);

                if (inst->isTranslationDelayed() &&
                    fault == NoFault) {
                    // A hw page table walk is currently going on; the
                    // instruction must be deferred.
                    DPRINTF(IEW, "Execute: Delayed translation, deferring "
                            "store.\n");
                    instQueue.deferMemInst(inst);
                    continue;
                }

                // If the store had a fault then it may not have a mem req
                if (fault != NoFault || !inst->readPredicate() ||
                        !inst->isStoreConditional()) {
                    // If the instruction faulted, then we need to send it
                    // along to commit without the instruction completing.
                    // Send this instruction to commit, also make sure iew
                    // stage realizes there is activity.
                    inst->setExecuted();
                    instToCommit(inst);
                    activityThisCycle();
                }

                // Store conditionals will mark themselves as
                // executed, and their writeback event will add the
                // instruction to the queue to commit.
            } else {
                panic("Unexpected memory type!\n");
            }

        } else {
            // If the instruction has already faulted, then skip executing it.
            // Such case can happen when it faulted during ITLB translation.
            // If we execute the instruction (even if it's a nop) the fault
            // will be replaced and we will lose it.
            if (inst->getFault() == NoFault) {
                inst->execute();
                if (!inst->readPredicate())
                    inst->forwardOldRegs();
            }

            inst->setExecuted();

            instToCommit(inst);
        }

        updateExeInstStats(inst);

        // C910 HPCP: count pipe issue by opClass → pipe mapping
        OpClass opc = inst->opClass();
        int pipeIdx = 0; // default: Pipe0 (IU ALU)
        switch (opc) {
          case enums::IntAlu:
            pipeIdx = 0; // Pipe0/Pipe1: IU ALU
            break;
          case enums::IntMult:
          case enums::IntDiv:
            pipeIdx = 1; // Pipe1: IU MLA/DIV
            break;
          case enums::FloatAdd:
          case enums::FloatCmp:
          case enums::FloatCvt:
          case enums::Bf16Cvt:
            pipeIdx = 6; // Pipe6: VFPU ALU (scalar FP)
            break;
          case enums::FloatMult:
          case enums::FloatMultAcc:
          case enums::FloatMisc:
            pipeIdx = 7; // Pipe7: VFPU MA
            break;
          case enums::FloatDiv:
          case enums::FloatSqrt:
            pipeIdx = 6; // Pipe6/7: FloatDiv/Sqrt
            break;
          case enums::SimdAdd:
          case enums::SimdAddAcc:
          case enums::SimdAlu:
          case enums::SimdCmp:
          case enums::SimdCvt:
          case enums::SimdMisc:
          case enums::SimdShift:
          case enums::SimdShiftAcc:
          case enums::SimdExt:
          case enums::SimdFloatExt:
          case enums::SimdConfig:
          case enums::SimdPredAlu:
          case enums::SimdFloatAdd:
          case enums::SimdFloatAlu:
          case enums::SimdFloatCmp:
          case enums::SimdFloatCvt:
          case enums::SimdFloatMisc:
          case enums::SimdReduceAdd:
          case enums::SimdReduceAlu:
          case enums::SimdReduceCmp:
          case enums::SimdFloatReduceAdd:
          case enums::SimdFloatReduceCmp:
          case enums::SimdDotProd:
          case enums::SimdAes:
          case enums::SimdAesMix:
          case enums::SimdSha1Hash:
          case enums::SimdSha1Hash2:
          case enums::SimdSha256Hash:
          case enums::SimdSha256Hash2:
          case enums::SimdShaSigma2:
          case enums::SimdShaSigma3:
          case enums::SimdSha3:
          case enums::SimdSm4e:
          case enums::SimdCrc:
          case enums::SimdBf16Add:
          case enums::SimdBf16Cmp:
          case enums::SimdBf16Cvt:
          case enums::MatrixMov:
            pipeIdx = 6; // Pipe6: VFPU ALU (vector ALU)
            break;
          case enums::SimdMult:
          case enums::SimdMultAcc:
          case enums::SimdMatMultAcc:
          case enums::SimdFloatMult:
          case enums::SimdFloatMultAcc:
          case enums::SimdFloatMatMultAcc:
          case enums::SimdDiv:
          case enums::SimdSqrt:
          case enums::SimdFloatDiv:
          case enums::SimdFloatSqrt:
          case enums::SimdBf16DotProd:
          case enums::SimdBf16MatMultAcc:
          case enums::SimdBf16Mult:
          case enums::SimdBf16MultAcc:
          case enums::Matrix:
          case enums::MatrixOP:
            pipeIdx = 7; // Pipe7: VFPU MA (vector MA)
            break;
          case enums::System:
            pipeIdx = 4; // Pipe4: LSU Special (FENCE/CSR)
            break;
          case enums::MemRead:
          case enums::FloatMemRead:
          case enums::SimdUnitStrideLoad:
          case enums::SimdUnitStrideMaskLoad:
          case enums::SimdUnitStrideSegmentedLoad:
          case enums::SimdStridedLoad:
          case enums::SimdIndexedLoad:
          case enums::SimdUnitStrideFaultOnlyFirstLoad:
          case enums::SimdUnitStrideSegmentedFaultOnlyFirstLoad:
          case enums::SimdWholeRegisterLoad:
          case enums::SimdStrideSegmentedLoad:
            pipeIdx = 3; // Pipe3: LSU Load
            break;
          case enums::MemWrite:
          case enums::FloatMemWrite:
          case enums::SimdUnitStrideStore:
          case enums::SimdUnitStrideMaskStore:
          case enums::SimdUnitStrideSegmentedStore:
          case enums::SimdStridedStore:
          case enums::SimdIndexedStore:
          case enums::SimdWholeRegisterStore:
          case enums::SimdStrideSegmentedStore:
            pipeIdx = 5; // Pipe5: LSU Store
            break;
          default:
            // RISC-V fences map to No_OpClass; detect via flags.
            if (opc == enums::No_OpClass) {
                if (inst->isReadBarrier() || inst->isWriteBarrier()) {
                    pipeIdx = 4; // Pipe4: LSU Special (fence)
                } else {
                    pipeIdx = 0; // default: Pipe0
                }
            } else {
                pipeIdx = 0;
            }
            break;
        }
        // Override pipe for control instructions (branches/jumps).
        // RISC-V branches may have opClass=IntAlu, so check flags after switch.
        if (inst->isDirectCtrl() || inst->isIndirectCtrl()) {
            pipeIdx = 2; // Pipe2: BJU
        }
        if (pipeIdx >= 0 && pipeIdx < 8) {
            iewStats.pipeIssueCount[pipeIdx]++;
        }
        if (opc == enums::System ||
            (opc == enums::No_OpClass &&
             (inst->isReadBarrier() || inst->isWriteBarrier()))) {
            iewStats.fenceSyncCount++;
        }

        // Check if branch prediction was correct, if not then we need
        // to tell commit to squash in flight instructions.  Only
        // handle this if there hasn't already been something that
        // redirects fetch in this group of instructions.

        // This probably needs to prioritize the redirects if a different
        // scheduler is used.  Currently the scheduler schedules the oldest
        // instruction first, so the branch resolution order will be correct.
        ThreadID tid = inst->threadNumber;

        if (!fetchRedirect[tid] ||
            !toCommit->squash[tid] ||
            toCommit->squashedSeqNum[tid] > inst->seqNum) {

            // Prevent testing for misprediction on load instructions,
            // that have not been executed.
            bool loadNotExecuted = !inst->isExecuted() && inst->isLoad();

            if (inst->mispredicted() && !loadNotExecuted) {
                fetchRedirect[tid] = true;

                DPRINTF(IEW, "[tid:%i] [sn:%llu] Execute: "
                        "Branch mispredict detected.\n",
                        tid, inst->seqNum);
                DPRINTF(IEW, "[tid:%i] [sn:%llu] "
                        "Predicted target was PC: %s\n",
                        tid, inst->seqNum, inst->readPredTarg());
                DPRINTF(IEW, "[tid:%i] [sn:%llu] Execute: "
                        "Redirecting fetch to PC: %s\n",
                        tid, inst->seqNum, inst->pcState());
                // If incorrect, then signal the ROB that it must be squashed.
                squashDueToBranch(inst, tid);

                ppMispredict->notify(inst);

                if (inst->readPredTaken()) {
                    iewStats.predictedTakenIncorrect++;
                } else {
                    iewStats.predictedNotTakenIncorrect++;
                }
            } else if (ldstQueue.violation(tid)) {
                assert(inst->isMemRef());
                // If there was an ordering violation, then get the
                // DynInst that caused the violation.  Note that this
                // clears the violation signal.
                DynInstPtr violator;
                violator = ldstQueue.getMemDepViolator(tid);

                DPRINTF(IEW, "LDSTQ detected a violation. Violator PC: %s "
                        "[sn:%lli], inst PC: %s [sn:%lli]. Addr is: %#x.\n",
                        violator->pcState(), violator->seqNum,
                        inst->pcState(), inst->seqNum, inst->physEffAddr);

                fetchRedirect[tid] = true;

                // Tell the instruction queue that a violation has occured.
                instQueue.violation(inst, violator);

                // Squash.
                squashDueToMemOrder(violator, tid);

                ++iewStats.memOrderViolationEvents;
            }
        } else {
            // Reset any state associated with redirects that will not
            // be used.
            if (ldstQueue.violation(tid)) {
                assert(inst->isMemRef());

                DynInstPtr violator = ldstQueue.getMemDepViolator(tid);

                DPRINTF(IEW, "LDSTQ detected a violation.  Violator PC: "
                        "%s, inst PC: %s.  Addr is: %#x.\n",
                        violator->pcState(), inst->pcState(),
                        inst->physEffAddr);
                DPRINTF(IEW, "Violation will not be handled because "
                        "already squashing\n");

                ++iewStats.memOrderViolationEvents;
            }
        }
    }

    // Update and record activity if we processed any instructions.
    if (inst_num) {
        if (exeStatus == Idle) {
            exeStatus = Running;
        }

        updatedQueues = true;

        cpu->activityThisCycle();
    }

    // Need to reset this in case a writeback event needs to write into the
    // iew queue.  That way the writeback event will write into the correct
    // spot in the queue.
    wbNumInst = 0;

}

void
IEW::writebackInsts()
{
    // Loop through the head of the time buffer and wake any
    // dependents.  These instructions are about to write back.  Also
    // mark scoreboard that this instruction is finally complete.
    // Writeback ports are classified by destination register type
    // (PREG/VREG/EREG) with independent port counts per type.
    unsigned pregWBCount = 0;
    unsigned vregWBCount = 0;
    unsigned eregWBCount = 0;

    for (int inst_num = 0; inst_num < wbWidth &&
             toCommit->insts[inst_num]; inst_num++) {
        DynInstPtr inst = toCommit->insts[inst_num];
        ThreadID tid = inst->threadNumber;

        DPRINTF(IEW, "Sending instructions to commit, [sn:%lli] PC %s.\n",
                inst->seqNum, inst->pcState());

        iewStats.instsToCommit[tid]++;
        // Notify potential listeners that execution is complete for this
        // instruction.
        ppToCommit->notify(inst);

        // Some instructions will be sent to commit without having
        // executed because they need commit to handle them.
        // E.g. Strictly ordered loads have not actually executed when they
        // are first sent to commit.  Instead commit must tell the LSQ
        // when it's ready to execute the strictly ordered load.
        if (!inst->isSquashed() && inst->isExecuted() &&
                inst->getFault() == NoFault) {

            // Count destination registers by PRF type to enforce per-port limits
            bool hasPregDest = false;
            bool hasVregDest = false;
            bool hasEregDest = false;

            for (int i = 0; i < inst->numDestRegs(); i++) {
                auto dest = inst->renamedDestIdx(i);
                if (dest->getNumPinnedWritesToComplete() == 0) {
                    switch (dest->classValue()) {
                      case IntRegClass:
                        hasPregDest = true;
                        break;
                      case VecRegClass:
                        hasVregDest = true;
                        break;
                      case FloatRegClass:
                        hasEregDest = true;
                        break;
                      default:
                        break;
                    }
                }
            }

            // Check per-PRF writeback port limits
            if ((hasPregDest && pregWBCount >= pregWBWidth) ||
                (hasVregDest && vregWBCount >= vregWBWidth) ||
                (hasEregDest && eregWBCount >= eregWBWidth)) {
                // This instruction exceeds per-PRF port limits;
                // stop processing further instructions this cycle.
                DPRINTF(IEW, "Writeback: PRF port limit reached "
                        "(preg:%u/%u, vreg:%u/%u, ereg:%u/%u)\n",
                        pregWBCount, pregWBWidth,
                        vregWBCount, vregWBWidth,
                        eregWBCount, eregWBWidth);
                break;
            }

            int dependents = instQueue.wakeDependents(inst);

            for (int i = 0; i < inst->numDestRegs(); i++) {
                // Mark register as ready if not pinned
                if (inst->renamedDestIdx(i)->
                        getNumPinnedWritesToComplete() == 0) {
                    DPRINTF(IEW,"Setting Destination Register %i (%s)\n",
                            inst->renamedDestIdx(i)->index(),
                            inst->renamedDestIdx(i)->className());
                    scoreboard->setReg(inst->renamedDestIdx(i));
                }
            }

            // Update per-PRF writeback port counts
            if (hasPregDest) pregWBCount++;
            if (hasVregDest) vregWBCount++;
            if (hasEregDest) eregWBCount++;

            if (dependents) {
                iewStats.producerInst[tid]++;
                iewStats.consumerInst[tid]+= dependents;
            }
            iewStats.writebackCount[tid]++;
        }
    }
}

void
IEW::tick()
{
    wbNumInst = 0;
    wbCycle = 0;

    wroteToTimeBuffer = false;
    updatedQueues = false;

    ldstQueue.tick();

    sortInsts();

    for (FUPool *fu_pool : fuPools) {
        // Free function units marked as being freed this cycle.
        fu_pool->processFreeUnits();
    }

    // Check stall and squash signals, dispatch any instructions.
    for (ThreadID tid : *activeThreads) {
        DPRINTF(IEW,"Issue: Processing [tid:%i]\n", tid);

        checkSignalsAndUpdate(tid);
        dispatch(tid);
    }

    if (exeStatus != Squashing) {
        executeInsts();

        writebackInsts();

        // Have the instruction queue try to schedule any ready instructions.
        // (In actuality, this scheduling is for instructions that will
        // be executed next cycle.)
        instQueue.scheduleReadyInsts();

        // Also should advance its own time buffers if the stage ran.
        // Not the best place for it, but this works (hopefully).
        issueToExecQueue.advance();
    }

    bool broadcast_free_entries = false;

    if (updatedQueues || exeStatus == Running || updateLSQNextCycle) {
        exeStatus = Idle;
        updateLSQNextCycle = false;

        broadcast_free_entries = true;
    }

    // Writeback any stores using any leftover bandwidth.
    ldstQueue.writebackStores();

    // Check the committed load/store signals to see if there's a load
    // or store to commit.  Also check if it's being told to execute a
    // nonspeculative instruction.
    // This is pretty inefficient...
    for (ThreadID tid : *activeThreads) {
        DPRINTF(IEW,"Processing [tid:%i]\n", tid);

        // Update structures based on instructions committed.
        if (fromCommit->commitInfo[tid].doneSeqNum != 0 &&
            !fromCommit->commitInfo[tid].squash &&
            !fromCommit->commitInfo[tid].robSquashing) {

            ldstQueue.commitStores(fromCommit->commitInfo[tid].doneSeqNum,tid);

            ldstQueue.commitLoads(fromCommit->commitInfo[tid].doneSeqNum,tid);

            updateLSQNextCycle = true;
            instQueue.commit(fromCommit->commitInfo[tid].doneSeqNum,tid);
        }

        if (fromCommit->commitInfo[tid].nonSpecSeqNum != 0) {

            //DPRINTF(IEW,"NonspecInst from thread %i",tid);
            if (fromCommit->commitInfo[tid].strictlyOrdered) {
                instQueue.replayMemInst(
                    fromCommit->commitInfo[tid].strictlyOrderedLoad);
                fromCommit->commitInfo[tid].strictlyOrderedLoad->setAtCommit();
            } else {
                instQueue.scheduleNonSpec(
                    fromCommit->commitInfo[tid].nonSpecSeqNum);
            }
        }

        if (broadcast_free_entries) {
            toFetch->iewInfo[tid].iqCount =
                instQueue.getCount(tid);
            toFetch->iewInfo[tid].ldstqCount =
                ldstQueue.getCount(tid);

            toRename->iewInfo[tid].usedIQ = true;
            toRename->iewInfo[tid].freeIQEntries =
                instQueue.numFreeEntries(tid);
            toRename->iewInfo[tid].usedLSQ = true;

            toRename->iewInfo[tid].freeLQEntries =
                ldstQueue.numFreeLoadEntries(tid);
            toRename->iewInfo[tid].freeSQEntries =
                ldstQueue.numFreeStoreEntries(tid);

            wroteToTimeBuffer = true;
        }

        DPRINTF(IEW, "[tid:%i], Dispatch dispatched %i instructions.\n",
                tid, toRename->iewInfo[tid].dispatched);
    }

    DPRINTF(IEW,
            "IQ has %i free entries (Can schedule: %i).  "
            "LQ has %i free entries. SQ has %i free entries.\n",
            instQueue.numFreeEntries(), instQueue.hasReadyInsts(),
            ldstQueue.numFreeLoadEntries(), ldstQueue.numFreeStoreEntries());

    updateStatus();

    // C910: Count stall types each cycle
    for (ThreadID tid : *activeThreads) {
        StallType stall = detectStallType(tid);
        iewStats.stallCycles[stall]++;
        if (stall == BACKEND_STALL) {
            iewStats.backendStallCycles++;
        }
        if (stall != NO_STALL) {
            iewStats.pipelineStallCycles++;
        }
    }

    // C910: Count IQ empty / pipeline empty cycles
    if (!instQueue.hasReadyInsts() && !ldstQueue.willWB()) {
        iewStats.iqEmptyCycles++;
        if (skidsEmpty()) {
            iewStats.pipelineEmptyCycles++;
        }
    }

    if (wroteToTimeBuffer) {
        DPRINTF(Activity, "Activity this cycle.\n");
        cpu->activityThisCycle();
    }
}

void
IEW::updateExeInstStats(const DynInstPtr& inst)
{
    ThreadID tid = inst->threadNumber;

    cpu->executeStats[tid]->numInsts++;

    inst->completeTick = curTick() - inst->fetchTick;

    //
    // ALU Operations
    //
    if (inst->isInteger()) {
        cpu->executeStats[tid]->numIntAluAccesses++;
    }

    if (inst->isFloating()) {
        cpu->executeStats[tid]->numFpAluAccesses++;
    }

    if (inst->isVector()) {
        cpu->executeStats[tid]->numVecAluAccesses++;
    }

    //
    //  Control operations
    //
    if (inst->isControl()) {
        cpu->executeStats[tid]->numBranches++;
    }

    //
    //  Memory operations
    //
    if (inst->isMemRef()) {
        cpu->executeStats[tid]->numMemRefs++;

        if (inst->isLoad()) {
            cpu->executeStats[tid]->numLoadInsts++;
        }
    }
}

void
IEW::checkMisprediction(const DynInstPtr& inst)
{
    ThreadID tid = inst->threadNumber;

    if (!fetchRedirect[tid] ||
        !toCommit->squash[tid] ||
        toCommit->squashedSeqNum[tid] > inst->seqNum) {

        if (inst->mispredicted()) {
            fetchRedirect[tid] = true;

            DPRINTF(IEW, "[tid:%i] [sn:%llu] Execute: "
                    "Branch mispredict detected.\n",
                    tid, inst->seqNum);
            DPRINTF(IEW, "[tid:%i] [sn:%llu] Predicted target was PC: %s\n",
                    tid, inst->seqNum, inst->readPredTarg());
            DPRINTF(IEW, "[tid:%i] [sn:%llu] Execute: "
                    "Redirecting fetch to PC: %s\n",
                    tid, inst->seqNum, inst->pcState());
            // If incorrect, then signal the ROB that it must be squashed.
            squashDueToBranch(inst, tid);

            if (inst->readPredTaken()) {
                iewStats.predictedTakenIncorrect++;
            } else {
                iewStats.predictedNotTakenIncorrect++;
            }
        }
    }
}

} // namespace o3
} // namespace gem5
