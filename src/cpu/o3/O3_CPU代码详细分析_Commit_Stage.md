# O3 CPU - Commit Stage 详细分析

## 概述

Commit（提交）阶段是 O3 CPU 流水线的最后阶段，负责按程序顺序提交指令的执行结果，确保**精确异常（Precise Exception）**语义。即使指令在流水线下游乱序执行，Commit 阶段保证指令以程序顺序对外可见。

### Commit 阶段的核心职责

1. **顺序提交**: 按程序顺序从 ROB 头部取出指令并提交
2. **异常处理**: 处理指令执行过程中产生的异常、陷阱、中断
3. **分支验证**: 确认分支预测结果，触发错误路径的 squash
4. **寄存器状态更新**: 更新提交点的重命名映射，释放已提交指令的物理寄存器
5. **内存操作提交**: 确保存储指令的数据实际写入内存系统

---

## 一、Commit 阶段架构

### 1.1 类定义 (commit.hh)

```cpp
class Commit
{
  public:
    enum CommitStatus {
        Active,     // 阶段活跃
        Inactive    // 阶段不活跃
    };

    enum ThreadStatus {
        Running,           // 正常提交
        Idle,              // 空闲
        ROBSquashing,      // ROB squash 中
        TrapPending,       // 等待 trap 处理
        FetchTrapPending,  // 等待取指 trap
        SquashAfterPending,// 等待 squashAfter 处理
        ThreadStatusMax
    };

  private:
    CommitStatus _status;
    CommitStatus _nextStatus;
    ThreadStatus commitStatus[MaxThreads];
    CommitPolicy commitPolicy;  // SMT 提交策略

    // ROB 指针
    ROB *rob;

    // 线程管理
    std::vector<ThreadState *> thread;
    std::list<ThreadID> *activeThreads;
    std::list<ThreadID> priority_list;

    // 提交点重命名映射
    UnifiedRenameMap *renameMap[MaxThreads];
    UnifiedRenameMap *commitRenameMap[MaxThreads];

    // PC 状态
    std::unique_ptr<PCStateBase> pc[MaxThreads];

    // 序列号追踪
    InstSeqNum youngestSeqNum[MaxThreads];
    InstSeqNum lastCommittedSeqNum[MaxThreads];

    // 状态标志
    bool wroteToTimeBuffer;
    bool changedROBNumEntries[MaxThreads];
    bool trapSquash[MaxThreads];
    bool tcSquash[MaxThreads];
    bool trapInFlight[MaxThreads];
    bool committedStores[MaxThreads];
    bool checkEmptyROB[MaxThreads];
    bool drainPending;
    bool drainImminent;
    bool canHandleInterrupts;
    bool avoidQuiesceLiveLock;

    // SquashAfter 支持
    DynInstPtr squashAfterInst[MaxThreads];

    // HTM (硬件事务内存) 支持
    int htmStarts[MaxThreads];
    int htmStops[MaxThreads];

    // 配置参数
    const Cycles iewToCommitDelay;
    const Cycles commitToIEWDelay;
    const Cycles renameToROBDelay;
    const Cycles fetchToCommitDelay;
    const unsigned renameWidth;
    const unsigned commitWidth;     // 每周期最大提交指令数
    const Cycles trapLatency;

    // 中断
    Fault interrupt;
};
```

### 1.2 线程状态说明

| 状态 | 说明 | 触发条件 |
|------|------|---------|
| `Running` | 正常提交 | 默认状态 |
| `Idle` | 无指令提交 | ROB 为空 |
| `ROBSquashing` | 正在 squash | 分支预测错误 |
| `TrapPending` | 等待 trap 处理 | 指令执行产生 trap |
| `FetchTrapPending` | 等待取指 trap | 取指阶段 trap |
| `SquashAfterPending` | 等待 squashAfter | 序列化指令的后续处理 |

---

## 二、核心数据结构

### 2.1 ROB - 重排序缓冲 (rob.hh)

```cpp
class ROB
{
  public:
    enum Status {
        Running,
        Idle,
        ROBSquashing
    };

  private:
    Status robStatus[MaxThreads];
    SMTQueuePolicy robPolicy;

    // 指令列表 (每线程)
    std::list<DynInstPtr> instList[MaxThreads];

    // 资源管理
    unsigned numEntries;
    unsigned threadEntries[MaxThreads];
    unsigned maxEntries[MaxThreads];

    // Squash 状态
    InstSeqNum squashedSeqNum[MaxThreads];
    bool doneSquashing[MaxThreads];

    // 可选的 squash 宽度
    const std::optional<unsigned> squashWidth;
};
```

### 2.2 ROB 关键操作

```cpp
// 获取 ROB 头部指令（最早未提交的指令）
DynInstPtr readHeadInst(ThreadID tid);

// 从 ROB 尾部获取指令（最晚进入的指令）
DynInstPtr readTailInst(ThreadID tid);

// 从 ROB 头部移除指令（提交后）
void retireHead(ThreadID tid);

// 插入指令到 ROB 尾部
void insertInst(const DynInstPtr &inst);

// 检查 ROB 是否为空
bool isEmpty(ThreadID tid);

// 检查 ROB 是否满
bool isFull(ThreadID tid);

// Squash 指令
void squash(InstSeqNum squash_num, ThreadID tid);
```

### 2.3 ROB 数据结构

```
ROB (每线程):
  ┌─────────────────────────────────────────────────┐
  │ head → [I1] → [I2] → [I3] → ... → [In] ← tail   │
  │          │                                      │
  │   最早进入，最先提交                              │
  │                          最新进入                │
  └─────────────────────────────────────────────────┘

提交方向: head → tail (程序顺序)
插入方向: 新指令插入 tail
Squash: 标记 seqNum > squash_num 的指令
```

---

## 三、tick() 主函数

### 3.1 执行流程

```cpp
void Commit::tick()
{
    bool status_change = false;

    // 1. 检查信号并更新各线程状态
    for (ThreadID tid = 0; tid < numThreads; tid++) {
        status_change = status_change || checkSignalsAndUpdate(tid);
    }

    // 2. 提交指令
    commit();

    // 3. 更新时间缓冲
    iewQueue->advance();
    renameQueue->advance();
    fetchQueue->advance();

    // 4. 更新总体状态
    if (status_change) {
        updateStatus();
    }
}
```

### 3.2 checkSignalsAndUpdate() - 信号处理

Commit 接收的信号：

```cpp
bool Commit::checkSignalsAndUpdate(ThreadID tid)
{
    bool status_change = false;

    // --- 来自 IEW 的 squash 信号 ---
    if (fromIEW->iewInfo[tid].squash) {
        InstSeqNum squashed_sn = fromIEW->iewInfo[tid].doneSeqNum;
        squash(squashed_sn, tid);
        status_change = true;
    }

    // --- 来自 IEW 的 trap 信号 ---
    if (fromIEW->iewInfo[tid].trapPending) {
        // 处理 trap
        trapSquash[tid] = true;
        status_change = true;
    }

    // --- 来自 IEW 的资源更新 ---
    if (fromIEW->iewInfo[tid].used) {
        // 更新 ROB 可用条目数
    }

    // --- 来自 Rename 的信号 ---
    if (fromRename->renameInfo[tid].used) {
        // 更新 Rename 到 ROB 的延迟状态
    }

    return status_change;
}
```

---

## 四、核心提交逻辑

### 4.1 commit() 函数

```cpp
void Commit::commit()
{
    // --- 处理 HTM 事务内存 ---
    for (ThreadID tid = 0; tid < numThreads; tid++) {
        if (executingHtmTransaction(tid)) {
            commitInsts();
            return;
        }
    }

    // --- 获取要提交的线程 (SMT 策略) ---
    ThreadID tid = getCommittingThread();

    if (tid != -1) {
        // --- 提交指令 ---
        commitInsts();
    }

    // --- 获取新指令到 ROB ---
    getInsts();

    // --- 标记已完成指令 ---
    markCompletedInsts();

    // --- 处理中断 ---
    handleInterrupt();
}
```

### 4.2 commitInsts() - 核心提交函数

```cpp
void Commit::commitInsts()
{
    unsigned num_committed = 0;

    while (num_committed < commitWidth) {
        // 选择提交线程
        ThreadID tid = getCommittingThread();

        if (tid == -1 || commitStatus[tid] != Running) {
            break;
        }

        // 读取 ROB 头部指令
        DynInstPtr head_inst = rob->readHeadInst(tid);

        if (!head_inst || !head_inst->readyToCommit()) {
            // 头部指令未完成，无法提交
            break;
        }

        // --- 分支预测错误检查 ---
        if (head_inst->isControl() &&
            (head_inst->misPredicted() ||
             (head_inst->isUncondCtrl() && !head_inst->traceData.taken))) {

            DPRINTF(Commit, "[tid:%i] Branch misprediction.\n", tid);

            // 发起 squash
            youngestSeqNum[tid] = head_inst->seqNum;
            commitStatus[tid] = ROBSquashing;

            toIEW->commitInfo[tid].squash = true;
            toIEW->commitInfo[tid].doneSeqNum = head_inst->seqNum;
            toIEW->commitInfo[tid].pc = &head_inst->pcState();
            toIEW->commitInfo[tid].branchTaken = head_inst->traceData.taken;
            toIEW->commitInfo[tid].mispredictInst = head_inst;

            wroteToTimeBuffer = true;
            stats.branchMispredicts++;

            break;
        }

        // --- 提交头部指令 ---
        bool committed = commitHead(head_inst, num_committed);

        if (!committed) {
            break;
        }

        num_committed++;
        lastCommittedSeqNum[tid] = head_inst->seqNum;

        // --- 从 ROB 移除头部 ---
        rob->retireHead(tid);

        // --- 更新统计 ---
        updateComInstStats(head_inst);
    }

    stats.numCommittedDist.sample(num_committed);
}
```

### 4.3 commitHead() - 提交单条指令

```cpp
bool Commit::commitHead(const DynInstPtr &head_inst, unsigned inst_num)
{
    ThreadID tid = head_inst->threadNumber;
    Fault fault = head_inst->getFault();

    // --- 处理指令错误 ---
    if (fault != NoFault) {
        DPRINTF(Commit, "Instruction fault: %s\n", fault->name());

        generateTrapEvent(tid, fault);

        if (fault == pageFault() || fault == alignmentFault()) {
            // 页面错误/对齐错误 - 需要 squash 重新执行
            toIEW->commitInfo[tid].doneSeqNum = head_inst->seqNum;
            toIEW->commitInfo[tid].squash = true;
            toIEW->commitInfo[tid].pc = &head_inst->pcState();
            toIEW->commitInfo[tid].branchTaken = false;
            toIEW->commitInfo[tid].mispredictInst = head_inst;

            wroteToTimeBuffer = true;
            return false;
        }

        return false;
    }

    // --- 清理重命名映射 ---
    // 将指令的目的寄存器映射返回给提交点的重命名映射
    for (int i = 0; i < head_inst->numDestRegs(); i++) {
        RegId arch_reg = head_inst->destRegIdx(i);
        PhysRegIdPtr phys_reg = head_inst->renameDestReg(i);

        // 更新提交点重命名映射
        commitRenameMap[tid]->setEntry(arch_reg, phys_reg);

        // 释放旧物理寄存器到空闲列表
        freeList->addReg(arch_reg.classType(), phys_reg);

        stats.committedMaps++;
    }

    // --- 标记指令已提交 ---
    head_inst->setCommitted();

    // --- 更新 PC ---
    pc[tid] = head_inst->pcState();

    DPRINTF(Commit, "[tid:%i] Committed [sn:%lli], PC %#x\n",
            tid, head_inst->seqNum, pc[tid]->instAddr());

    return true;
}
```

---

## 五、ROB 管理

### 5.1 getInsts() - 从 IEW 接收指令到 ROB

```cpp
void Commit::getInsts()
{
    // 从 IEW 阶段接收已写回的指令
    for (ThreadID tid = 0; tid < numThreads; tid++) {
        if (fromIEW->iewInfo[tid].size > 0) {
            for (int i = 0; i < fromIEW->iewInfo[tid].size; i++) {
                DynInstPtr inst = fromIEW->iewInfo[tid].insts[i];
                rob->insertInst(inst);
            }
        }
    }
}
```

### 5.2 markCompletedInsts() - 标记已完成指令

```cpp
void Commit::markCompletedInsts()
{
    for (ThreadID tid = 0; tid < numThreads; tid++) {
        if (fromIEW->iewInfo[tid].size > 0) {
            for (int i = 0; i < fromIEW->iewInfo[tid].size; i++) {
                DynInstPtr inst = fromIEW->iewInfo[tid].insts[i];
                inst->setCanCommit();  // 标记指令可以提交
            }
        }
    }
}
```

### 5.3 squash() - ROB Squash

```cpp
void Commit::squash(const InstSeqNum &squash_num, ThreadID tid)
{
    DPRINTF(Commit, "[tid:%i] Squashing until seq_num %i.\n",
            tid, squash_num);

    // 标记所有 seqNum > squash_num 的指令为已 squash
    int squash_count = 0;
    bool done_squashing = true;

    for (auto it = rob->instList[tid].begin();
         it != rob->instList[tid].end(); ++it) {
        if ((*it)->seqNum > squash_num) {
            (*it)->setSquashed();
            squash_count++;

            // 如果设置了 squashWidth，限制每周期 squash 数量
            if (squashWidth) {
                if (squash_count >= *squashWidth) {
                    done_squashing = false;
                    break;
                }
            }
        }
    }

    if (done_squashing) {
        doneSquashing[tid] = true;
    }

    youngestSeqNum[tid] = squash_num;
    commitStatus[tid] = ROBSquashing;
}
```

---

## 六、精确异常机制

### 6.1 异常处理流程

```
指令执行产生 Fault
        │
        ▼
    Fault 存储在 DynInst 中
        │
        ▼
    指令到达 ROB 头部
        │
        ▼
    commitHead() 检查 fault
        │
        ├── NoFault → 正常提交
        │
        └── 有 Fault → generateTrapEvent()
                │
                ├── 页面错误 → squash 并重新取指
                ├── 对齐错误 → squash 并重新取指
                ├── 系统调用 → 处理系统调用
                └── 其他 trap → 交给 OS 处理
```

### 6.2 generateTrapEvent()

```cpp
void Commit::generateTrapEvent(ThreadID tid, const Fault &fault)
{
    DPRINTF(Commit, "[tid:%i] Generating trap event.\n", tid);

    // 设置 trap 延迟
    trapInFlight[tid] = true;

    // squash 从 trap 指令之后的所有指令
    toIEW->commitInfo[tid].squash = true;
    toIEW->commitInfo[tid].doneSeqNum = head_inst->seqNum;
    toIEW->commitInfo[tid].pc = &head_inst->pcState();

    wroteToTimeBuffer = true;
    stats.traps++;
}
```

### 6.3 handleInterrupt() - 中断处理

```cpp
void Commit::handleInterrupt()
{
    // 检查是否有待处理的中断
    for (ThreadID tid = 0; tid < numThreads; tid++) {
        if (canHandleInterrupts && commitStatus[tid] == Running) {
            Fault interrupt = thread[tid]->getInterrupt();

            if (interrupt != NoFault) {
                // 处理中断
                generateTrapEvent(tid, interrupt);
            }
        }
    }
}
```

---

## 七、SMT 提交策略

### 7.1 策略类型

```cpp
enum class CommitPolicy {
    RoundRobin,    // 轮询
    IQCount,       // 根据 IQ 空闲条目
    LSQCount,      // 根据 LSQ 空闲条目
};
```

### 7.2 getCommittingThread()

```cpp
ThreadID Commit::getCommittingThread()
{
    // 根据 SMT 策略选择下一个提交的线程
    // 确保各线程公平地提交指令
    switch (commitPolicy) {
      case CommitPolicy::RoundRobin:
        return roundRobin();
      // ... 其他策略
    }
    return -1;
}
```

---

## 八、阶段间通信

### 8.1 输入（来自 IEW）

```cpp
struct IEWStruct {
    struct {
        DynInstPtr insts[MaxIEWWidth];
        int size;
        ThreadID tid;
        bool trapPending;
        bool used;
    } iewInfo[MaxThreads];
};
```

通过 `iewQueue` 时间缓冲接收已写回指令。

### 8.2 输出（到 IEW/Decode/Rename/Fetch）

```cpp
struct CommitStruct {
    struct {
        bool squash;
        InstSeqNum doneSeqNum;
        const PCStateBase *pc;
        bool branchTaken;
        DynInstPtr mispredictInst;
        bool used;
    } commitInfo[MaxThreads];
};
```

通过 `timeBuffer` 反向发送 squash 信号到所有上游阶段。

### 8.3 信号传播路径

```
Commit squash 信号
        │
        ├──▶ IEW    → squash IQ/LSQ 中的指令
        │
        ├──▶ Rename → 恢复重命名映射
        │
        ├──▶ Decode → 清空译码队列
        │
        ├──▶ Fetch  → 更新 PC，清空取指队列
        │
        └──▶ BAC    → squash BPU 历史，更新 BAC PC
```

---

## 九、提交流程图

```
┌─────────────────────────────────────────────────────────────┐
│                     Commit Stage                             │
│                                                              │
│  1. checkSignalsAndUpdate()                                  │
│     ├── squash 信号 → squash ROB                             │
│     └── trap 信号 → 处理 trap                                │
│                                                              │
│  2. commit()                                                 │
│     ├── getCommittingThread() → 选择提交线程                 │
│     ├── commitInsts()                                        │
│     │   ├── readHeadInst() → 读 ROB 头部指令                 │
│     │   ├── readyToCommit? → 检查是否就绪                    │
│     │   ├── 分支预测检查 → 不匹配则 squash                   │
│     │   ├── commitHead() → 提交指令                          │
│     │   │   ├── 检查 fault → trap 处理                      │
│     │   │   ├── 更新 commitRenameMap                         │
│     │   │   ├── freeList.addReg() → 释放物理寄存器           │
│     │   │   ├── inst.setCommitted()                          │
│     │   │   └── 更新 PC                                      │
│     │   └── retireHead() → 从 ROB 移除                       │
│     │                                                        │
│     ├── getInsts() → 从 IEW 接收指令到 ROB                   │
│     ├── markCompletedInsts() → 标记可提交                    │
│     └── handleInterrupt() → 处理中断                         │
│                                                              │
└─────────────────────────────────────────────────────────────┘
```

---

## 十、关键参数

| 参数 | 说明 |
|------|------|
| `commitWidth` | 每周期最大提交指令数 |
| `ROBEntries` | ROB 总条目数 |
| `squashWidth` | 每周期最大 squash 指令数 (可选) |
| `trapLatency` | trap 处理延迟 |
| `iewToCommitDelay` | IEW 到 Commit 延迟 |
| `commitToIEWDelay` | Commit 反传到 IEW 延迟 |
| `renameToROBDelay` | Rename 到 ROB 延迟 |
| `fetchToCommitDelay` | Fetch 到 Commit 延迟 |

---

## 十一、关键设计要点

### 11.1 精确异常的保证

Commit 阶段是 O3 CPU 实现精确异常的关键：

1. **程序顺序提交**: 即使指令在 IEW 阶段乱序执行，Commit 始终从 ROB 头部（最早进入的指令）开始提交
2. **异常点精确**: 当指令产生异常时，该指令是 ROB 头部的指令，所有之前的指令已提交，所有之后的指令未提交
3. **状态可恢复**: 通过 squash 机制，可以从异常点之后的所有指令全部清除，从正确的 PC 重新取指

### 11.2 寄存器回收

```
重命名时的寄存器分配:
  freeList → 分配新物理寄存器 → 指令使用

提交时的寄存器回收:
  commitHead() → 旧物理寄存器 → freeList.addReg()

关键: 只有提交后，旧的物理寄存器才会被回收
      这保证了未提交的指令仍然可以访问正确的旧值
```

### 11.3 Store 操作的提交

存储指令的数据实际写入发生在 Commit 阶段：
1. IEW 阶段计算存储地址和数据，暂存在 LSQ 中
2. Commit 阶段按程序顺序提交存储指令
3. 存储指令提交后，数据才实际写入 D-cache/内存

这确保了：
- 如果指令被 squash，存储操作不会产生可见效果
- 存储操作按程序顺序执行

### 11.4 Drain 机制

```cpp
bool drainPending;     // 是否有 drain 请求
bool drainImminent;    // drain 是否即将完成
```

Drain 用于安全地暂停 CPU（如检查点保存、系统调用切换）：
1. 停止取指
2. 等待流水线中所有指令提交
3. 确认 ROB 为空
4. CPU 进入暂停状态

---

## 生成信息

- **分析范围**: Commit Stage (commit.hh/cc, rob.hh/cc)
- **gem5 版本**: v25.1.0.0
