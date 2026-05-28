# Commit Stage 规格说明

## 1. 概述

Commit（提交）阶段是 O3 CPU 流水线的最后一级，负责：

1. 从 Rename 接收已重命名的指令，插入 ROB（Reorder Buffer）
2. 按序提交（退休）已完成的指令，确保架构状态的顺序更新
3. 处理 squash 和分支预测错误的最终确认
4. 处理 trap、中断和系统调用
5. 寄存器回收：释放被替换的旧物理寄存器
6. 精确异常：确保异常在正确的架构点触发
7. Drain 机制：支持 CPU 上下文切换和检查点保存

## 2. 类接口

### 2.1 `class Commit`

**头文件**: `src/cpu/o3/commit.hh`

#### 构造函数
```cpp
Commit(CPU *_cpu, const BaseO3CPUParams &params);
```

#### 核心方法

| 方法 | 说明 |
|------|------|
| `void tick()` | 每周期主入口；处理 squash、获取指令、提交指令 |
| `void startupStage()` | 初始化阶段，广播 ROB 空闲条目 |
| `void clearStates(ThreadID tid)` | 清除指定线程的全部 Commit 状态 |
| `void drain()` / `void drainResume()` | 排空/恢复机制 |
| `bool isDrained() const` | 检查流水线是否完全排空 |
| `void drainSanityCheck() const` | 排空后的健全性检查 |
| `void takeOverFrom()` | 从另一个 CPU 的线程状态接管 |
| `void deactivateThread(ThreadID tid)` | 将线程从活跃列表移除 |
| `void setTimeBuffer(TimeBuffer<TimeStruct> *tb_ptr)` | 设置反向通信时间缓冲 |
| `void setFetchQueue(TimeBuffer<FetchStruct> *fq_ptr)` | 设置来自 Fetch 的队列 |
| `void setRenameQueue(TimeBuffer<RenameStruct> *rq_ptr)` | 设置来自 Rename 的队列 |
| `void setIEWQueue(TimeBuffer<IEWStruct> *iq_ptr)` | 设置来自 IEW 的队列 |
| `void setIEWStage(IEW *iew_stage)` | 设置 IEW 阶段指针 |
| `void setRenameMap(UnifiedRenameMap *rm_ptr)` | 设置已提交的 rename map |
| `void setROB(ROB *rob_ptr)` | 设置 ROB 指针 |
| `size_t numROBFreeEntries(ThreadID tid)` | 获取指定线程的 ROB 空闲条目 |

## 3. 数据结构

### 3.1 状态枚举

**CommitStatus（总体状态）：**
| 状态 | 说明 |
|------|------|
| `Active` | 阶段活跃 |
| `Inactive` | 阶段不活跃 |

**ThreadStatus（每线程状态）：**
| 状态 | 说明 | 触发条件 |
|------|------|---------|
| `Running` | 正常提交 | 默认状态 |
| `Idle` | 空闲 | 无指令可提交 |
| `ROBSquashing` | 正在 squash ROB | 收到 squash 信号 |
| `TrapPending` | 等待 trap 处理 | trap 事件已调度 |
| `FetchTrapPending` | 等待取指 trap | 取指阶段的 trap |
| `SquashAfterPending` | squashAfter 等待中 | 上周期提交了 squashAfter 指令 |

### 3.2 关键成员变量

```cpp
CommitStatus _status;
CommitStatus _nextStatus;                          // 下周期总体状态
ThreadStatus commitStatus[MaxThreads];             // 每线程状态
CommitPolicy commitPolicy;                         // SMT 提交策略

// ROB
ROB *rob;

// 时间缓冲接口
TimeBuffer<TimeStruct> *timeBuffer;
TimeBuffer<TimeStruct>::wire toIEW;                // 发送到 IEW
TimeBuffer<TimeStruct>::wire robInfoFromIEW;       // 从 IEW 读取（延迟后）

TimeBuffer<FetchStruct> *fetchQueue;
TimeBuffer<FetchStruct>::wire fromFetch;           // 从 Fetch 读取

TimeBuffer<IEWStruct> *iewQueue;
TimeBuffer<IEWStruct>::wire fromIEW;               // 从 IEW 读取

TimeBuffer<RenameStruct> *renameQueue;
TimeBuffer<RenameStruct>::wire fromRename;         // 从 Rename 读取

// PC 跟踪
std::unique_ptr<PCStateBase> pc[MaxThreads];

// 序列号跟踪
InstSeqNum youngestSeqNum[MaxThreads];             // ROB 中最年轻指令的序列号
InstSeqNum lastCommitedSeqNum[MaxThreads];         // 最后提交的序列号

// 事件跟踪
bool trapSquash[MaxThreads];                       // trap 导致的 squash
bool tcSquash[MaxThreads];                         // TC 写导致的 squash
bool trapInFlight[MaxThreads];                     // 是否有未处理的 trap
bool committedStores[MaxThreads];                  // 本周期是否有 store 提交
bool checkEmptyROB[MaxThreads];                   // 是否需要检查 ROB 为空

DynInstPtr squashAfterInst[MaxThreads];            // squashAfter 指令缓冲

// 中断
bool canHandleInterrupts;                          // 是否可以处理中断
bool avoidQuiesceLiveLock;                         // 避免静默活锁
Fault interrupt;                                   // 当前中断 fault

// Drain
bool drainPending;                                 // 是否有排空请求
bool drainImminent;                                // 排空是否即将完成

// HTM
int htmStarts[MaxThreads];                         // HTM 事务开始计数
int htmStops[MaxThreads];                          // HTM 事务结束计数

// SMT
std::list<ThreadID> priority_list;                 // 优先级列表
ThreadID numThreads;
std::list<ThreadID> *activeThreads;

// Rename map（已提交状态）
UnifiedRenameMap *renameMap[MaxThreads];

// 配置参数
const Cycles iewToCommitDelay;                     // IEW 到 Commit 的延迟
const Cycles commitToIEWDelay;                     // Commit 反传到 IEW 的延迟
const Cycles renameToROBDelay;                     // Rename 到 ROB 的延迟
const Cycles fetchToCommitDelay;                   // Fetch 到 Commit 的延迟
const unsigned renameWidth;                        // 重命名宽度（ROB 插入宽度）
const unsigned commitWidth;                        // 提交宽度
const Cycles trapLatency;                          // Trap 处理延迟

// Probe
ProbePointArg<DynInstPtr> *ppCommit;               // 提交探针
ProbePointArg<DynInstPtr> *ppCommitStall;          // 提交停顿探针
ProbePointArg<DynInstPtr> *ppSquash;               // Squash 探针
```

## 4. Tick 行为

### 4.1 主流程

```
tick()
  ├─ wroteToTimeBuffer = false
  ├─ _nextStatus = Inactive
  │
  ├─ 1. 检查 squash 完成状态:
  │    └─ 遍历每个活跃线程:
  │         ├─ 清空 committedStores[tid]
  │         └─ if commitStatus[tid] == ROBSquashing:
  │              ├─ if rob->isDoneSquashing(tid):
  │              │    └─ commitStatus[tid] = Running
  │              └─ else:
  │                   ├─ rob->doSquash(tid)          // 继续批量 squash
  │                   └─ toIEW->commitInfo[tid].robSquashing = true
  │
  ├─ 2. commit()                                     // 核心提交逻辑
  │
  ├─ 3. markCompletedInsts()                         // 标记 IEW 完成的指令
  │
  ├─ 4. 检查 ROB 头部指令状态:
  │    └─ if ROB 头部指令 readyToCommit:
  │         └─ _nextStatus = Active
  │
  └─ 5. updateStatus()                               // 更新总体状态
```

### 4.2 `commit()` - 核心提交逻辑

```
commit()
  ├─ 【步骤 1】处理 squash（优先级最高）:
  │    └─ 遍历每个活跃线程:
  │         ├─ if trapSquash[tid]: squashFromTrap(tid)
  │         ├─ if tcSquash[tid]: squashFromTC(tid)
  │         ├─ if SquashAfterPending: squashFromSquashAfter(tid)
  │         └─ if fromIEW->squash[tid] (来自 IEW 的 squash):
  │              ├─ 验证序列号 <= youngestSeqNum[tid]
  │              ├─ commitStatus[tid] = ROBSquashing
  │              ├─ rob->squash(squash_seq_num, tid)
  │              ├─ toIEW->commitInfo[tid].squash = true
  │              ├─ toIEW->commitInfo[tid].mispredictInst = fromIEW->mispredictInst
  │              └─ 如果分支预测错误: stats.branchMispredicts++
  │
  ├─ 【步骤 2】如果所有线程都在 squash，跳过后续步骤
  │
  ├─ 【步骤 3】getInsts(): 从 Rename 获取指令插入 ROB
  │
  └─ 【步骤 4】commitInsts(): 尝试提交指令
```

## 5. ROB 管理

### 5.1 `getInsts()` - 插入 ROB

```cpp
void Commit::getInsts()
{
    // 从 Rename 队列读取已重命名的指令
    int insts_to_process = min(renameWidth, fromRename->size);

    for (int i = 0; i < insts_to_process; i++) {
        const DynInstPtr &inst = fromRename->insts[i];
        ThreadID tid = inst->threadNumber;

        if (!inst->isSquashed() &&
            commitStatus[tid] != ROBSquashing &&
            commitStatus[tid] != TrapPending) {

            rob->insertInst(inst);                   // 插入 ROB
            youngestSeqNum[tid] = inst->seqNum;      // 更新最年轻序列号
        }
    }
}
```

### 5.2 `markCompletedInsts()` - 标记完成指令

```cpp
void Commit::markCompletedInsts()
{
    // 从 IEW 队列获取已执行完成的指令
    for (int i = 0; i < fromIEW->size; i++) {
        if (!fromIEW->insts[i]->isSquashed()) {
            fromIEW->insts[i]->setCanCommit();       // 标记可提交
        }
    }
}
```

### 5.3 ROB 状态

| 操作 | 说明 | 触发条件 |
|------|------|---------|
| `insertInst(inst)` | 插入指令到 ROB 尾部 | 从 Rename 接收指令 |
| `retireHead(tid)` | 移除 ROB 头部指令 | 指令成功提交 |
| `squash(seqNum, tid)` | 标记 squash 点 | 分支预测错误、trap |
| `doSquash(tid)` | 执行批量 squash | 每周期移除多条 squashed 指令 |
| `isDoneSquashing(tid)` | 检查 squash 是否完成 | tick 中检查 |
| `readHeadInst(tid)` | 读取 ROB 头部指令 | 提交检查 |
| `isHeadReady(tid)` | ROB 头部指令是否可提交 | 提交检查 |
| `isEmpty(tid)` | ROB 是否为空 | 排空检查 |
| `numFreeEntries(tid)` | ROB 空闲条目数 | 反压上游 |

## 6. 指令提交

### 6.1 `commitInsts()` - 提交循环

```
commitInsts()
  └─ while (num_committed < commitWidth):
       ├─ 【选择线程】getCommittingThread():
       │    ├─ 优先选择正在退出的线程（exit syscall）
       │    └─ 根据 SMT 策略选择：
       │         ├─ RoundRobin → roundRobin()
       │         └─ OldestReady → oldestReady()
       │
       ├─ 【检查 ROB 头部】rob->isHeadReady(tid):
       │    └─ 未就绪 → break
       │
       └─ head_inst = rob->readHeadInst(tid)
       │
       ├─ 【情况 1】指令已 squash:
       │    └─ rob->retireHead(tid)
       │       └─ stats.commitSquashedInsts++
       │
       ├─ 【情况 2】CPU 无对应功能单元:
       │    └─ panic()  ← 配置错误
       │
       └─ 【情况 3】正常提交:
            └─ commitHead(head_inst, num_committed):
                 ├─ 如果指令未执行（非推测/严格有序）:
                 │    ├─ 通知 IEW 执行: toIEW->nonSpecSeqNum
                 │    └─ return false  ← 等待 IEW 执行
                 │
                 ├─ 如果指令有 fault:
                 │    ├─ 等待所有 store 写回完成
                 │    ├─ cpu->trap(fault)  ← 执行 trap
                 │    └─ commitStatus = TrapPending
                 │
                 └─ 提交成功:
                      ├─ 更新 commit rename map
                      ├─ rob->retireHead(tid)
                      ├─ toIEW->doneSeqNum = head_inst->seqNum
                      ├─ 更新 misc. registers
                      ├─ head_inst->staticInst->advancePC(*pc[tid])
                      ├─ lastCommitedSeqNum[tid] = head_inst->seqNum
                      ├─ 如果是 squashAfter 指令 → squashAfter(tid)
                      └─ 如果是 drainPending → squashAfter + drainImminent
```

### 6.2 `commitHead()` - 提交单条指令

```cpp
bool Commit::commitHead(const DynInstPtr &head_inst, unsigned inst_num)
{
    ThreadID tid = head_inst->threadNumber;

    // 【检查 1】指令未执行（非推测指令）
    if (!head_inst->isExecuted()) {
        assert(head_inst->isNonSpeculative() || ...);

        // 等待所有 store 写回
        if (inst_num > 0 || iewStage->hasStoresToWB(tid)) {
            return false;
        }

        // 通知 IEW 执行
        toIEW->commitInfo[tid].nonSpecSeqNum = head_inst->seqNum;
        head_inst->clearCanCommit();

        // 严格有序 load 特殊处理
        if (head_inst->isLoad() && head_inst->strictlyOrdered()) {
            toIEW->commitInfo[tid].strictlyOrderedLoad = head_inst;
        }

        return false;  // 等待 IEW 执行完成
    }

    // 【检查 2】指令 fault
    Fault inst_fault = head_inst->getFault();
    if (inst_fault != NoFault) {
        // 等待 store 写回
        if (iewStage->hasStoresToWB(tid) || inst_num > 0) {
            return false;
        }

        cpu->trap(inst_fault, tid, head_inst->staticInst);
        commitStatus[tid] = TrapPending;
        generateTrapEvent(tid, inst_fault);
        return false;
    }

    // 【检查 3】正常提交
    // 更新 commit rename map（释放旧物理寄存器）
    for (int i = 0; i < head_inst->numDestRegs(); i++) {
        renameMap[tid]->setEntry(head_inst->flattenedDestIdx(i),
                                 head_inst->renamedDestIdx(i));
    }

    rob->retireHead(tid);

    // 更新 misc. registers
    head_inst->updateMiscRegs();

    // 更新 PC
    head_inst->staticInst->advancePC(*pc[tid]);

    // 更新序列号
    lastCommitedSeqNum[tid] = head_inst->seqNum;

    // 检查 squashAfter
    if (head_inst->isSquashAfter()) {
        squashAfter(tid, head_inst);
    }

    return true;
}
```

## 7. Squash 处理

### 7.1 Squash 类型

| 类型 | 触发条件 | 处理方法 |
|------|---------|---------|
| **Trap Squash** | trap 事件触发 | `squashFromTrap()` |
| **TC Squash** | 线程上下文写 | `squashFromTC()` |
| **SquashAfter** | squashAfter 指令提交 | `squashFromSquashAfter()` |
| **Branch Mispredict** | IEW 报告预测错误 | `commit()` 中处理 |
| **Memory Violation** | IEW 报告内存序违例 | `commit()` 中处理 |

### 7.2 `squashAll()` - Squash 所有指令

```cpp
void Commit::squashAll(ThreadID tid)
{
    // 计算 squash 序列号
    InstSeqNum squashed_inst = rob->isEmpty(tid) ?
        lastCommitedSeqNum[tid] : rob->readHeadInst(tid)->seqNum - 1;

    youngestSeqNum[tid] = lastCommitedSeqNum[tid];

    rob->squash(squashed_inst, tid);

    // 向 IEW 发送 squash 信号
    toIEW->commitInfo[tid].doneSeqNum = squashed_inst;
    toIEW->commitInfo[tid].squash = true;
    toIEW->commitInfo[tid].robSquashing = true;
    set(toIEW->commitInfo[tid].pc, pc[tid]);
}
```

### 7.3 `squashAfter()` - 两阶段 Squash

`squashAfter` 指令要求提交该指令后 squash 所有后续指令。由于 commit 和 squash 不能在同一周期进行，分为两个阶段：

```
阶段 1（当前周期）:
  └─ squashAfter(tid, head_inst):
       └─ commitStatus[tid] = SquashAfterPending
       └─ squashAfterInst[tid] = head_inst

阶段 2（下一周期 tick 中）:
  └─ commitStatus[tid] == SquashAfterPending:
       └─ squashFromSquashAfter(tid):
            └─ squashAll(tid)
            └─ toIEW->squashInst = squashAfterInst[tid]
```

### 7.4 Squash 优先级

```
1. Trap Squash（最高） - trapSquash[tid]
2. TC Squash          - tcSquash[tid]
3. SquashAfter        - commitStatus == SquashAfterPending
4. IEW Squash（最低）  - fromIEW->squash[tid]
```

注意：IEW squash 必须验证 `squashedSeqNum <= youngestSeqNum`，防止年轻的 squash 覆盖老的 squash。

## 8. Trap 处理

### 8.1 Trap 事件流程

```
指令触发 trap (commitHead 中检测到 fault)
  └─ cpu->trap(fault, tid, staticInst)     // 立即执行 trap，更新架构状态
  └─ commitStatus[tid] = TrapPending
  └─ generateTrapEvent(tid, fault):
       └─ 调度 EventFunctionWrapper，延迟 trapLatency 周期
       └─ trapInFlight[tid] = true
       └─ toIEW->commitInfo[tid].trapPending = true

trapLatency 周期后:
  └─ processTrapEvent(tid):
       └─ trapSquash[tid] = true

下一周期 tick 中:
  └─ squashFromTrap(tid):
       └─ squashAll(tid)
       └─ thread[tid]->trapPending = false
       └─ commitStatus[tid] = ROBSquashing
```

### 8.2 Trap 处理的关键点

- **立即更新架构状态**：`cpu->trap()` 在检测到 fault 时立即执行，而非等待 trap 事件触发
- **延迟 squash**：trap 事件延迟 `trapLatency` 周期后才发起 squash，模拟 trap 处理的延迟
- **`noSquashFromTC` 保护**：trap 执行期间禁止 squash，防止 TC 操作触发额外的 squash

## 9. 中断处理

### 9.1 中断传播

```cpp
void Commit::propagateInterrupt()
{
    // 如果已有中断处理中，跳过
    if (commitStatus[0] == TrapPending || interrupt || ...) return;

    interrupt = cpu->getInterrupts();

    if (interrupt != NoFault) {
        toIEW->commitInfo[0].interruptPending = true;  // 通知 Fetch 停止取指
    }
}
```

### 9.2 中断处理

```cpp
void Commit::handleInterrupt()
{
    // 检查中断是否仍然有效（可能被请求者清除）
    if (!cpu->checkInterrupts(0)) {
        toIEW->commitInfo[0].clearInterrupt = true;
        interrupt = NoFault;
        avoidQuiesceLiveLock = true;
        return;
    }

    // 等待所有在途指令完成
    if (canHandleInterrupts && cpu->instList.empty()) {
        toIEW->commitInfo[0].clearInterrupt = true;

        cpu->processInterrupts(cpu->getInterrupts());

        commitStatus[0] = TrapPending;
        generateTrapEvent(0, interrupt);
    }
}
```

### 9.3 避免静默活锁

当指令启用中断后，如果有之前被屏蔽的中断 pending，需要立即 squash 以避免活锁：

```cpp
if (!interrupt && avoidQuiesceLiveLock &&
    onInstBoundary && cpu->checkInterrupts(0)) {
    squashAfter(tid, head_inst);
}
```

## 10. SMT 提交策略

| 策略 | 选择方法 |
|------|---------|
| `RoundRobin` | 按优先级列表轮询 |
| `OldestReady` | 选择序列号最小的就绪线程 |

**优先级规则：** 正在退出（exit syscall）的线程优先于所有策略，确保其所有 squashed 指令能在本周期内完成退休。

### RoundRobin 实现

```cpp
ThreadID Commit::roundRobin()
{
    for (auto it = priority_list.begin(); it != priority_list.end(); ++it) {
        ThreadID tid = *it;
        if (commitStatus[tid] 可提交 && rob->isHeadReady(tid)) {
            priority_list.erase(it);
            priority_list.push_back(tid);
            return tid;
        }
    }
    return InvalidThreadID;
}
```

### OldestReady 实现

```cpp
ThreadID Commit::oldestReady()
{
    unsigned oldest = 0, oldest_seq_num = 0;
    bool first = true;

    for (ThreadID tid : *activeThreads) {
        if (commitStatus[tid] 可提交 && rob->isHeadReady(tid)) {
            if (first || head_inst->seqNum < oldest_seq_num) {
                oldest = tid;
                oldest_seq_num = head_inst->seqNum;
                first = false;
            }
        }
    }
    return first ? InvalidThreadID : oldest;
}
```

## 11. Drain 机制

### 11.1 排空流程

```
drain()
  └─ drainPending = true

drainSanityCheck()
  └─ 确保:
       ├─ ROB 为空
       ├─ 无中断 pending
       └─ 所有线程 PC 的 microPC == 0（不在微码序列中间）

isDrained()
  └─ rob->isEmpty() && interrupt == NoFault
```

### 11.2 排空触发

当 `drainPending` 为 true 时，在下一个指令边界触发：

```cpp
if (drainPending) {
    if (pc[tid]->microPC() == 0 && interrupt == NoFault &&
        !thread[tid]->trapPending) {
        // 最后一条架构指令
        squashAfter(tid, head_inst);
        cpu->commitDrained(tid);
        drainImminent = true;  // 禁用中断，等待结构排空
    }
}
```

## 12. HTM（硬件事务内存）

```cpp
// 跟踪 HTM 事务嵌套深度
if (head_inst->isHtmStart()) htmStarts[tid]++;
if (head_inst->isHtmStop()) htmStops[tid]++;

// 判断是否在执行事务中
bool executingHtmTransaction(ThreadID tid) {
    return htmStarts[tid] > htmStops[tid];
}

// HTM 事务中延迟中断处理
if (executingHtmTransaction(commit_thread)) {
    cpu->clearInterrupts(0);
    interrupt = NoFault;
}

// HTM fault 处理
if (inst_fault != NoFault && head_inst->inHtmTransactionalState()) {
    // 转换为 GenericHtmFailureFault
    inst_fault = std::make_shared<GenericHtmFailureFault>(
        head_inst->getHtmTransactionUid(),
        HtmFailureFaultCause::EXCEPTION);
}
```

## 13. 寄存器回收

Commit 阶段通过更新 commit rename map 来回收旧物理寄存器：

```cpp
// 在 commitHead() 成功后:
for (int i = 0; i < head_inst->numDestRegs(); i++) {
    renameMap[tid]->setEntry(
        head_inst->flattenedDestIdx(i),    // 架构寄存器
        head_inst->renamedDestIdx(i)       // 新物理寄存器
    );
    // 旧物理寄存器自动被标记为可回收
    // UnifiedFreeList 将其加入空闲列表
}
```

## 14. 配置参数

| 参数 | 类型 | 说明 |
|------|------|------|
| `commitWidth` | unsigned | 每周期最大提交指令数 |
| `renameWidth` | unsigned | 每周期最大从 Rename 接收指令数 |
| `iewToCommitDelay` | Cycles | IEW 到 Commit 的延迟 |
| `commitToIEWDelay` | Cycles | Commit 反传到 IEW 的延迟 |
| `renameToROBDelay` | Cycles | Rename 到 ROB 的延迟 |
| `commitToFetchDelay` | Cycles | Commit 到 Fetch 的延迟 |
| `trapLatency` | Cycles | Trap 处理延迟 |
| `numThreads` | ThreadID | SMT 线程数 |
| `smtCommitPolicy` | CommitPolicy | SMT 提交策略 |

## 15. 统计信息

| 统计项 | 类型 | 说明 |
|--------|------|------|
| `status` | Vector | 各 Commit 状态消耗的周期数 |
| `commitSquashedInsts` | Scalar | Commit 跳过的 squashed 指令数 |
| `commitNonSpecStalls` | Scalar | 非推测指令导致的停顿次数 |
| `branchMispredicts` | Scalar | 分支预测错误次数 |
| `numCommittedDist` | Distribution | 每周期提交指令数分布 |
| `amos` | Vector | 已提交的原子指令数（每线程） |
| `membars` | Vector | 已提交的内存屏障数（每线程） |
| `committedInstType` | Vector2d | 按操作类型分类的提交指令数 |
| `commitEligibleSamples` | Scalar | 达到提交带宽上限的周期数 |

## 16. Probe 探针点

| 探针 | 负载类型 | 触发条件 |
|------|---------|---------|
| `Commit` | `DynInstPtr` | 指令被成功提交 |
| `CommitStall` | `DynInstPtr` | 提交被阻塞 |
| `Squash` | `DynInstPtr` | 指令被 squash |

## 17. 阶段间通信

### 17.1 输入：来自 Rename

通过 `renameQueue` TimeBuffer 接收：

```cpp
struct RenameStruct {
    int size;
    DynInstPtr insts[MaxWidth];
};
```

### 17.2 输入：来自 IEW

通过 `iewQueue` TimeBuffer 接收：

```cpp
struct IEWStruct {
    int size;
    DynInstPtr insts[MaxWidth];
    DynInstPtr mispredictInst[MaxThreads];
    InstSeqNum squashedSeqNum[MaxThreads];
    bool squash[MaxThreads];
    bool branchMispredict[MaxThreads];
    bool includeSquashInst[MaxThreads];
    ...
};
```

### 17.3 输出：到 IEW/Fetch/Decode/Rename

通过 `TimeStruct::CommitComm` 广播：

| 字段 | 消费者 | 说明 |
|------|--------|------|
| `pc` | Fetch | 重定向 PC |
| `mispredictInst` | Fetch | 预测错误的指令 |
| `squashInst` | Fetch | 导致 squash 的指令 |
| `strictlyOrderedLoad` | IEW | 严格有序 load |
| `nonSpecSeqNum` | IEW | 非推测指令序列号 |
| `doneSeqNum` | IEW, Fetch | 已提交/已 squash 序列号 |
| `freeROBEntries` | Rename | ROB 空闲条目 |
| `squash` | 全部 | Squash 信号 |
| `robSquashing` | 全部 | ROB 正在 squash |
| `usedROB` | Rename | ROB 使用情况 |
| `emptyROB` | Rename | ROB 为空 |
| `branchTaken` | Fetch | 分支方向 |
| `interruptPending` | Fetch | 中断挂起 |
| `clearInterrupt` | Fetch | 清除中断 |
| `trapPending` | Fetch | Trap 挂起 |
| `strictlyOrdered` | IEW | 严格有序访问 |

## 18. 流水线位置

```
┌──────────┐  renameQueue   ┌──────────┐
│  Rename  │ ─────────────▶ │  Commit  │
│          │                │          │
│ - 重命名  │                │ - 插入ROB│
│ - 转发指令               │ - 按序提交│
└──────────┘                │ - 精确异常│
                            │ - 寄存器回收│
                            │ - Drain    │
                            └─────┬────┘
                                  │
                    ┌─────────────┼──────────────┐
                    ▼             ▼              ▼
                  IEW          Fetch          Rename
               (doneSeqNum)  (nextPC)     (freeROBEntries)
```

## 19. 关键设计要点

1. **按序提交保证精确异常** —— 无论指令如何乱序执行，Commit 阶段严格按 ROB 顺序提交，确保异常发生在正确的架构点。

2. **两阶段 SquashAfter** —— squashAfter 指令需要先提交自身，再 squash 后续指令。由于 commit 和 squash 不能同周期进行，使用 `SquashAfterPending` 状态分为两个周期完成。

3. **Trap 立即更新架构状态** —— 检测到 fault 时立即调用 `cpu->trap()` 更新架构状态，延迟 `trapLatency` 后再发起 squash。这虽然时序上不完全准确，但确保 trap 处理期间不会因 TC 操作触发额外的 squash。

4. **序列号优先级** —— IEW squash 信号必须验证 `squashedSeqNum <= youngestSeqNum`，防止年轻的 squash 信号覆盖老的 squash 信号。

5. **ROB 空检查延迟** — `emptyROB` 标志不仅要求 ROB 为空，还要求所有 store 已写回且 IEW 已接收 store 信息，防止上游过早认为 ROB 可用。

6. **Interrupt 活锁避免** — 当指令启用中断后有 pending 的中断时，立即触发 squashAfter，避免"启用中断 → 中断 pending → 被屏蔽 → 再次启用"的活锁循环。

7. **Drain 保证微码边界** —— 排空时确保 `microPC == 0`，保证 CPU 不在微码序列中间被暂停，简化了 CPU 切换和检查点恢复的实现。
