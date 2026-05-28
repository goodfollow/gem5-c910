# gem5 O3 CPU Instruction Queue (IQ) 模块规格说明书

## 1. 概述

Instruction Queue (IQ) 是 gem5 O3 CPU 中 IEW (Issue/Execute/Writeback) 阶段的核心组件，负责：
- 存储已译码并重命名的指令
- 跟踪指令间的寄存器依赖关系
- 调度就绪指令到功能单元 (FU) 执行
- 支持乱序执行 (Out-of-Order Execution)

### 1.1 模块位置

```
O3 CPU 流水线架构:
┌─────────────┐    ┌─────────────┐    ┌─────────────┐    ┌─────────────┐    ┌─────────────┐
│   Fetch     │ -> │   Decode    │ -> │   Rename    │ -> │    IEW      │ -> │   Commit    │
│   (取指)    │    │   (译码)    │    │   (重命名)  │    │ (发射/执行) │    │   (提交)    │
└─────────────┘    └─────────────┘    └─────────────┘    └─────────────┘    └─────────────┘
                                                          │
                                                          ├─── Instruction Queue (IQ)
                                                          ├─── Load/Store Queue (LSQ)
                                                          └─── Functional Unit Pool (FUPool)
```

## 2. 架构设计

### 2.1 组件层次结构

```
InstructionQueue (顶层)
│
├── IQUnit[] (指令队列单元，支持 SMT 多线程)
│   ├── iqPolicy: SMTQueuePolicy (Dynamic/Partitioned/Threshold)
│   ├── maxEntries[MaxThreads]
│   ├── count[MaxThreads]
│   └── _fuPool: FUPool*
│
├── DependencyGraph (依赖图)
│   └── dependGraph[numPhysRegs] (每物理寄存器一条依赖链)
│
├── MemDepUnit[MaxThreads] (内存依赖单元)
│   ├── StoreSet
│   └── 内存屏障跟踪
│
├── ReadyInstQueue[Num_OpClasses] (就绪队列，按操作类型)
│
└── TimeBuffer 接口
    ├── issueToExecuteQueue (发射到执行)
    └── timeBuffer (反向通信)
```

### 2.2 核心数据结构

#### 2.2.1 DependencyGraph (依赖图)

```cpp
template <class DynInstPtr>
class DependencyGraph {
    // 数组的链表，每个物理寄存器一条链
    std::vector<DepEntry> dependGraph;  // numEntries = numPhysRegs

    struct DepEntry {
        DynInstPtr inst;       // 生产者指令（链头）或消费者指令
        DepEntry *next;        // 指向下一个消费者
    };
};
```

**设计要点**：
- 每个物理寄存器对应一条依赖链
- 链头节点：生产者指令（将写入该寄存器的指令）
- 后续节点：消费者指令（需要读取该寄存器值的指令）
- 当生产者执行完成时，唤醒所有消费者

#### 2.2.2 ReadyInstQueue (就绪优先级队列)

```cpp
struct PqCompare {
    // 按序列号逆序排序，序列号小的（老指令）在队顶
    bool operator()(const DynInstPtr &lhs, const DynInstPtr &rhs) const;
};

typedef std::priority_queue<
    DynInstPtr, std::vector<DynInstPtr>, PqCompare> ReadyInstQueue;

ReadyInstQueue readyInsts[Num_OpClasses];  // 按操作类型分离
```

**操作类型分类**：
- `IntAluOp`: 整数 ALU 操作
- `FloatAddOp`, `FloatCmpOp`, `FloatCvtOp`, `FloatDivOp`, `FloatMultOp`, `FloatSqrtOp`: FP 操作
- `SimdAddOp`, `SimdCmpOp`, `SimdCvtOp`, `SimdDivOp`, `SimdMultOp`, `SimdSqrtOp`, `SimdAluOp`, `SimdShiftOp`, `SimdMiscOp`, `SimdReductionOp`, `SimdFlagOp`: SIMD/向量操作
- `MemReadOp`, `MemWriteOp`, `MemMiscOp`: 内存操作
- `IprOp`, `MiscOp`, `NopOp`: 特殊操作

#### 2.2.3 ListOrder (年龄排序列表)

```cpp
struct ListOrderEntry {
    OpClass queueType;      // 就绪队列类型
    InstSeqNum oldestInst;  // 该队列中最老指令的序列号
};

std::list<ListOrderEntry> listOrder;  // 按最老指令序列号排序
```

**作用**：跨操作类型选择最老的就绪指令，维护指令间的全局顺序。

### 2.3 SMT 资源分配策略

| 策略 | 描述 | IQ 分配方式 |
|------|------|-------------|
| `Dynamic` | 动态共享 | 所有线程共享全部 IQ 条目 |
| `Partitioned` | 分区分配 | IQ 条目均匀分配给各线程 |
| `Threshold` | 阈值控制 | 单线程可用全部，多线程按阈值分配 |

## 3. 输入输出信号

### 3.1 输入信号（从其他模块到 IQ）

#### 3.1.1 来自 Rename 模块

| 信号 | 类型 | 描述 |
|------|------|------|
| `insert(new_inst)` | 控制 | 插入新译码指令到 IQ |
| `insertNonSpec(new_inst)` | 控制 | 插入非推测指令（如 barrier） |
| `insertBarrier(barr_inst)` | 控制 | 插入内存屏障指令 |
| `scheduleNonSpec(seqNum)` | 控制 | 调度非推测指令执行 |

#### 3.1.2 来自 Commit 模块（通过 TimeBuffer）

| 信号 | 类型 | 描述 |
|------|------|------|
| `doneSeqNum` | 数据 | 已提交/已结束的序列号 |
| `squash[tid]` | 控制 |  squash 信号，使无效路径指令 |
| `nonSpecSeqNum` | 数据 | 非推测指令的序列号 |

#### 3.1.3 来自 IEW/LSQ 模块

| 信号 | 类型 | 描述 |
|------|------|------|
| `violation(store, load)` | 控制 | 内存顺序违例，触发 squash |
| `rescheduleMemInst(inst)` | 控制 | 重调度内存指令 |
| `replayMemInst(inst)` | 控制 | 重放内存指令 |
| `deferMemInst(inst)` | 控制 | 延迟内存指令（页表遍历） |
| `blockMemInst(inst)` | 控制 | 阻塞内存指令（cache 阻塞） |
| `retryMemInst(inst)` | 控制 | 重试内存指令 |
| `cacheUnblocked()` | 控制 | cache 解除阻塞通知 |

#### 3.1.4 来自 Functional Unit

| 信号 | 类型 | 描述 |
|------|------|------|
| `FUCompletion` 事件 | 事件 | FU 执行完成事件 |

### 3.2 输出信号（从 IQ 到其他模块）

#### 3.2.1 到 IEW/Execute 模块

| 信号 | 类型 | 描述 |
|------|------|------|
| `getInstToExecute()` | 数据 | 提供待执行指令 |
| `getDeferredMemInstToExecute()` | 数据 | 提供延迟完成的内存指令 |
| `getBlockedMemInstToExecute()` | 数据 | 提供阻塞解除的内存指令 |
| `hasReadyInsts()` | 状态 | IQ 是否有就绪指令 |
| `numFreeEntries()` | 状态 | 空闲 IQ 条目数 |
| `isFull()` | 状态 | IQ 是否已满 |

#### 3.2.2 到 Commit 模块

| 信号 | 类型 | 描述 |
|------|------|------|
| `wakeDependents(completed_inst)` | 控制 | 唤醒完成指令的依赖者 |

#### 3.2.3 到 Rename 模块（通过 TimeBuffer）

| 信号 | 类型 | 描述 |
|------|------|------|
| `freeIQEntries` | 状态 | 空闲 IQ 条目数 |
| `freeLQEntries` | 状态 | 空闲 Load Queue 条目数 |
| `freeSQEntries` | 状态 | 空闲 Store Queue 条目数 |
| `dispatchedToLQ` | 计数 | dispatch 到 LQ 的指令数 |
| `dispatchedToSQ` | 计数 | dispatch 到 SQ 的指令数 |
| `usedIQ` | 标志 | IQ 状态是否已使用 |
| `usedLSQ` | 标志 | LSQ 状态是否已使用 |

### 3.3 信号时序图

```
Cycle N:                          Cycle N+1:                        Cycle N+2:

Rename → IQ:                      IQ 内部:                          IQ → Execute:
┌──────────────────┐              ┌──────────────────┐              ┌──────────────────┐
│ insert(inst)     │─────────────>│ 1. 加入依赖图     │─────────────>│ getInstToExecute()│
│                  │              │ 2. 标记生产者     │              │ (就绪指令)       │
│                  │              │ 3. 检查是否就绪   │              │                  │
│                  │              │ 4. 加入就绪队列   │              │                  │
└──────────────────┘              └──────────────────┘              └──────────────────┘

Commit → IQ (反向):               IQ 内部:
┌──────────────────┐              ┌──────────────────┐
│ doneSeqNum       │─────────────>│ 1. 移除已完成指令 │
│ squash[tid]      │─────────────>│ 2. squashing      │
└──────────────────┘              └──────────────────┘
```

## 4. 流水线设计

### 4.1 流水线级数

gem5 O3 的 IQ 模块**不是传统意义上的固定流水线**，而是采用**事件驱动的动态调度机制**。但我们可以将其操作分解为以下逻辑阶段：

```
┌─────────────────────────────────────────────────────────────────────────┐
│                        IQ 逻辑流水线（4 级）                              │
├─────────────┬─────────────┬─────────────┬───────────────────────────────┤
│   Stage 0   │   Stage 1   │   Stage 2   │          Stage 3              │
│  指令插入   │  依赖跟踪   │  就绪调度   │        执行完成               │
│  (Insert)   │  (Track)    │  (Schedule) │      (Complete)               │
├─────────────┼─────────────┼─────────────┼───────────────────────────────┤
│ • 接收指令  │ • 查询依赖图 │ • 选择最老   │ • FU 完成事件                  │
│ • 分配 IQ 项  │ • 注册生产者 │   就绪指令  │ • 唤醒消费者                  │
│ • 加入 instList│ • 更新记分牌 │ • 分配 FU   │ • 释放 FU 资源                 │
│             │ • 加入 memDep  │ • 送入执行队列│ • 送入提交队列              │
└─────────────┴─────────────┴─────────────┴───────────────────────────────┘
```

### 4.2 各级详细功能

#### Stage 0: 指令插入 (Insert)

**输入**：来自 Rename 的 `DynInstPtr`

**处理流程**：
```cpp
void InstructionQueue::insert(const DynInstPtr &new_inst)
{
    // 1. 更新统计
    if (new_inst->isFloating()) {
        iqIOStats.fpInstQueueWrites++;
    } else if (new_inst->isVector()) {
        iqIOStats.vecInstQueueWrites++;
    } else {
        iqIOStats.intInstQueueWrites++;
    }

    // 2. 加入线程指令列表
    instList[new_inst->threadNumber].push_back(new_inst);

    // 3. 分配 IQ 条目
    auto iq = findIQ(new_inst);
    iq->insert(new_inst);  // freeEntries--, count[tid]++

    // 4. 依赖跟踪（Stage 1）
    addToDependents(new_inst);
    addToProducers(new_inst);

    // 5. 内存指令特殊处理
    if (new_inst->isMemRef()) {
        memDepUnit[new_inst->threadNumber].insert(new_inst);
    } else {
        addIfReady(new_inst);  // 非内存指令直接检查就绪
    }

    iqStats.instsAdded++;
}
```

**输出**：
- 指令进入 `instList[tid]`
- IQ 条目计数更新
- 依赖关系建立

#### Stage 1: 依赖跟踪 (Track)

**1.1 消费者注册 (`addToDependents`)**：

```cpp
bool addToDependents(const DynInstPtr &new_inst)
{
    // 遍历指令的所有源寄存器
    for (int src_reg : new_inst->srcRegIndices()) {
        // 检查寄存器是否就绪（通过 scoreboard）
        if (!regScoreboard[src_reg]) {
            // 寄存器未就绪，加入依赖图等待
            dependGraph.insert(src_reg, new_inst);
            new_inst->setNotReady(src_reg);
        }
        // 寄存器已就绪，无需等待
    }
}
```

**1.2 生产者注册 (`addToProducers`)**：

```cpp
void addToProducers(const DynInstPtr &new_inst)
{
    // 遍历指令的所有目的寄存器
    for (int dst_reg : new_inst->dstRegIndices()) {
        // 设置该寄存器依赖图的链头为当前指令
        dependGraph.setInst(dst_reg, new_inst);
        // 标记寄存器为"将由此指令产生"
        new_inst->setProducer(dst_reg);
    }
}
```

**数据结构**：
```
物理寄存器 R1:  [Producer: inst_100] -> [Consumer: inst_105] -> [Consumer: inst_108] -> NULL
物理寄存器 R2:  [Producer: inst_102] -> [Consumer: inst_110] -> NULL
物理寄存器 R3:  NULL (无生产者，无消费者)
```

#### Stage 2: 就绪调度 (Schedule)

**2.1 检查就绪 (`addIfReady`)**：

```cpp
void addIfReady(const DynInstPtr &inst)
{
    // 检查所有源寄存器是否都已就绪
    if (inst->allSrcRegsReady()) {
        // 所有依赖已满足，加入就绪队列
        readyInsts[inst->opClass()].push(inst);

        // 将该 OpClass 加入年龄排序列表
        if (!queueOnList[inst->opClass()]) {
            addToOrderList(inst->opClass());
        }
    }
}
```

**2.2 调度就绪指令 (`scheduleReadyInsts`)**：

```cpp
void scheduleReadyInsts()
{
    int total_issued = 0;
    ListOrderIt order_it = listOrder.begin();

    // 遍历年龄排序列表，选择最老的就绪指令
    while (total_issued < totalWidth && order_it != order_end_it) {
        OpClass op_class = (*order_it).queueType;
        DynInstPtr issuing_inst = readyInsts[op_class].top();

        // 跳过已 squashed 的指令
        if (issuing_inst->isSquashed()) {
            readyInsts[op_class].pop();
            if (!readyInsts[op_class].empty()) {
                moveToYoungerInst(order_it);
            }
            continue;
        }

        // 获取功能单元
        int fu_idx = fuPool->getUnit(op_class);

        if (fu_idx >= 0) {
            // 成功分配 FU，发送指令到执行
            readyInsts[op_class].pop();

            // 更新年龄排序列表
            if (!readyInsts[op_class].empty()) {
                moveToYoungerInst(order_it);
            } else {
                readyIt[op_class] = listOrder.erase(order_it);
                queueOnList[op_class] = false;
            }

            // 设置 FU 完成事件
            issueToExecuteQueue->access(0)->insts[total_issued] = issuing_inst;
            issuing_inst->execute();  // 原子执行

            // 调度 FU 完成事件（在 execute_latency 后触发）
            FUCompletion *completion = new FUCompletion(
                issuing_inst, fuPool, fu_idx, this);
            schedule(completion, clockEdge(execute_latency));

            total_issued++;
            iqStats.instsIssued++;
        } else {
            // 无可用 FU，跳过此 OpClass
            order_it++;
            iqStats.statFuBusy[op_class]++;
        }
    }

    numIssuedDist.sample(total_issued);
}
```

**2.3 唤醒依赖者 (`wakeDependents`)**：

```cpp
int wakeDependents(const DynInstPtr &completed_inst)
{
    int dependents_woke = 0;

    // 遍历完成指令的所有目的寄存器
    for (int dst_reg : completed_inst->dstRegIndices()) {
        // 从依赖图弹出所有消费者
        while (!dependGraph.empty(dst_reg)) {
            DynInstPtr dependent = dependGraph.pop(dst_reg);

            // 标记该源寄存器已就绪
            dependent->markSrcRegReady(dst_reg);

            // 检查指令是否完全就绪
            if (dependent->allSrcRegsReady()) {
                addIfReady(dependent);
            }

            dependents_woke++;
        }

        // 清除寄存器记分牌
        regScoreboard[dst_reg] = false;
    }

    return dependents_woke;
}
```

#### Stage 3: 执行完成 (Complete)

**3.1 FU 完成事件处理**：

```cpp
void processFUCompletion(const DynInstPtr &inst, FUPool *fu_pool, int fu_idx)
{
    // 减少在途指令计数
    --wbOutstanding;

    // 唤醒 CPU（如果处于休眠）
    iewStage->wakeCPU();

    // 释放 FU 资源
    if (fu_pool && fu_idx > -1) {
        fu_pool->freeUnitNextCycle(fu_idx);
    }

    // 将指令加入执行完成队列（送到 Writeback）
    issueToExecuteQueue->access(-1)->size++;
    instsToExecute.push_back(inst);
}
```

**3.2 获取待执行指令**：

```cpp
DynInstPtr getInstToExecute()
{
    assert(!instsToExecute.empty());
    DynInstPtr inst = std::move(instsToExecute.front());
    instsToExecute.pop_front();

    // 更新统计
    if (inst->isFloating()) {
        iqIOStats.fpInstQueueReads++;
    } else if (inst->isVector()) {
        iqIOStats.vecInstQueueReads++;
    } else {
        iqIOStats.intInstQueueReads++;
    }

    return inst;
}
```

### 4.3 流水线延迟参数

| 参数 | 默认值 | 描述 |
|------|--------|------|
| `commitToIEWDelay` | 3 cycles | Commit 到 IEW 的反向通信延迟 |
| `renameToIEWDelay` | 1 cycle | Rename 到 IEW 的延迟 |
| `issueToExecuteDelay` | 1 cycle | Issue 到 Execute 的延迟 |
| `issueWidth` | 配置相关 | 每周期最大发射指令数 |
| `dispatchWidth` | 配置相关 | 每周期最大 dispatch 指令数 |
| `wbWidth` | 配置相关 | 每周期最大 writeback 指令数 |

### 4.4 时序波形图

```
Cycle:        1         2         3         4         5         6
              │         │         │         │         │         │
Stage 0:      │inst_A   │inst_B   │         │         │         │
  Insert      │插入     │插入     │         │         │         │
              │         │         │         │         │         │
Stage 1:      │         │dep_A    │dep_B    │         │         │
  Track       │         │建立依赖 │建立依赖 │         │         │
              │         │         │         │         │         │
Stage 2:      │         │         │sched_A  │sched_B  │         │
  Schedule    │         │         │发射 A    │发射 B    │         │
              │         │         │         │         │         │
Stage 3:      │         │         │         │         │cmp_A    │
  Complete    │         │         │         │         │A 完成唤醒│
              │         │         │         │         │         │
              │         │         │         │         │sched_C  │
              │         │         │         │         │(C 依赖 A)│
```

## 5. 内存依赖处理

### 5.1 MemDepUnit 架构

```
MemDepUnit (每线程一个)
│
├── StoreSet (存储集预测器)
│   ├── SSIT (Store Set Identifier Table)
│   ├── LFST (Last Fetched Store Table)
│   └── 预测 load 依赖于哪些 store
│
├── pendingLoads: list<DynInstPtr>  (等待的 load)
├── pendingStores: list<DynInstPtr> (等待的 store)
└── barrierList: list<DynInstPtr>   (内存屏障)
```

### 5.2 内存操作状态转换

```
                    ┌─────────────────────────────────────────┐
                    │                                         ▼
┌──────────┐   insert   ┌──────────────┐  schedule  ┌─────────────────┐
│  Rename  │───────────>│ MemDepUnit   │───────────>│ IQ Ready Queue  │
└──────────┘            │ 插入内存指令  │            │ (等待执行)      │
                        └──────────────┘            └────────┬────────┘
                                                             │
              ┌──────────────────────────────────────────────┤
              │                                              │
              ▼                                              ▼
    ┌─────────────────┐                           ┌─────────────────┐
    │ deferredMemInsts│                           │  execute on FU  │
    │ (页表遍历延迟)   │                           │                 │
    └────────┬────────┘                           └────────┬────────┘
             │                                             │
             │ getDeferredMemInstToExecute()               │ complete
             ▼                                             ▼
    ┌─────────────────┐                           ┌─────────────────┐
    │ blockedMemInsts │                           │ wakeDependents  │
    │ (cache 阻塞)     │                           │ (唤醒消费者)     │
    └────────┬────────┘                           └─────────────────┘
             │
             │ getBlockedMemInstToExecute()
             │ retryMemInst()
             ▼
    ┌─────────────────┐
    │ retryMemInsts   │
    │ (可重试)        │
    └─────────────────┘
```

### 5.3 内存顺序违例处理

```cpp
void violation(const DynInstPtr &store, const DynInstPtr &faulting_load)
{
    // 检测到 store 和 load 之间的顺序违例
    // store 在 load 之后执行，但地址相同

    // 触发 squash，从 faulting_load 开始重新执行
    iewStage->squashDueToMemOrder(faulting_load, load->threadNumber);

    iqStats.memOrderViolationEvents++;
}
```

## 6. 统计信息

### 6.1 IQ 性能统计

| 统计项 | 类型 | 描述 |
|--------|------|------|
| `instsAdded` | Scalar | 加入 IQ 的指令数（不含非推测） |
| `nonSpecInstsAdded` | Scalar | 加入的非推测指令数 |
| `instsIssued` | Scalar | 发射的指令总数 |
| `intInstsIssued` | Scalar | 发射的整数指令数 |
| `floatInstsIssued` | Scalar | 发射的浮点指令数 |
| `branchInstsIssued` | Scalar | 发射的分支指令数 |
| `memInstsIssued` | Scalar | 发射的内存指令数 |
| `squashedInstsIssued` | Scalar | 被 squashed 但仍发射的指令数 |
| `squashedInstsExamined` | Scalar | squash 时检查的指令数 |
| `squashedOperandsExamined` | Scalar | squash 时检查的操作数 |
| `squashedNonSpecRemoved` | Scalar | 因 squash 移除的非推测指令数 |
| `numIssuedDist` | Distribution | 每周期发射指令数分布 |
| `fuBusy` | Vector | FU 忙事件计数（每线程） |
| `fuBusyRate` | Formula | FU 忙率 = fuBusy / instsIssued |
| `issuedInstType` | Vector2D | 按 FU 类型和线程分类的发射数 |
| `issueRate` | Formula | 发射率 = instsIssued / numCycles |

### 6.2 IQ IO 统计

| 统计项 | 类型 | 描述 |
|--------|------|------|
| `intInstQueueReads` | Scalar | 整数 IQ 读次数 |
| `intInstQueueWrites` | Scalar | 整数 IQ 写次数 |
| `intInstQueueWakeupAccesses` | Scalar | 整数 IQ 唤醒访问次数 |
| `fpInstQueueReads/Writes/WakeupAccesses` | Scalar | 浮点 IQ 访问次数 |
| `vecInstQueueReads/Writes/WakeupAccesses` | Scalar | 向量 IQ 访问次数 |
| `intAluAccesses` | Scalar | 整数 ALU 访问次数 |
| `fpAluAccesses` | Scalar | 浮点 ALU 访问次数 |
| `vecAluAccesses` | Scalar | 向量 ALU 访问次数 |

## 7. 配置参数

### 7.1 IQ 相关参数

```python
# IQ 配置 (在 O3CPU.py 或架构特定文件中)
class IQUnitParams(SimObject):
    numEntries = 64              # IQ 总条目数
    numIQs = 1                   # IQ 单元数量
    smtIQPolicy = "Partitioned"  # SMT 分配策略
    smtIQThreshold = 50          # 阈值策略的百分比阈值
    fuPool = FUPool()            # 功能单元池

# 全局 O3 参数
class BaseO3CPUParams(CPUParams):
    # IQ/IEW 延迟
    commitToIEWDelay = 3         # Commit 到 IEW 延迟
    renameToIEWDelay = 1         # Rename 到 IEW 延迟
    issueToExecuteDelay = 1      # Issue 到 Execute 延迟

    # 宽度参数
    dispatchWidth = 2            # Dispatch 宽度
    issueWidth = 2               # Issue 宽度
    wbWidth = 2                  # Writeback 宽度

    # 缓冲区大小
    backComSize = 5              # 反向通信缓冲区大小
    forwardComSize = 5           # 前向通信缓冲区大小

    # 寄存器文件
    numPhysIntRegs = 256         # 物理整数寄存器数
    numPhysFloatRegs = 256       # 物理浮点寄存器数
    numPhysVecRegs = 256         # 物理向量寄存器数

    # 功能单元池
    fuPool = FUPool()            # FU 池配置
```

### 7.2 FUPool 配置

```python
class FUPoolParams(ParamObject):
    opList = [
        FUDesc(opClass="IntAlu", count=2, opLat=1, issueLat=1, pipelined=True),
        FUDesc(opClass="FloatAdd", count=1, opLat=2, issueLat=1, pipelined=True),
        FUDesc(opClass="FloatMult", count=1, opLat=4, issueLat=1, pipelined=True),
        FUDesc(opClass="MemRead", count=1, opLat=2, issueLat=1, pipelined=True),
        FUDesc(opClass="MemWrite", count=1, opLat=2, issueLat=1, pipelined=True),
    ]
```

## 8. 关键设计决策

### 8.1 为什么 IQ 不是固定流水线？

gem5 O3 的 IQ 采用**事件驱动**而非固定流水线的设计，原因：
1. **乱序执行需求**：指令就绪时间不确定，无法用固定级数流水线处理
2. **依赖跟踪复杂性**：依赖关系是图状结构，不是线性流程
3. **功能单元多样性**：不同操作类型的延迟差异很大

### 8.2 背压 (Backpressure) 机制

```
当 IQ 满时:
┌──────────────┐    IQ Full    ┌──────────────┐
│    Rename    │──────────────>│     IEW      │
│              │<──────────────│              │
│              │  block 信号    │              │
└──────────────┘               └──────────────┘

实现:
dispatchStatus[tid] = Blocked
toRename->iewBlock[tid] = true
```

### 8.3 Squash 机制

```
Squash 传播路径:
Commit ──squash[tid]──> IQ ──> instList 中所有指令标记为 squashed
                       │
                       └──> 依赖图中移除 squashed 指令
                       │
                       └──> nonSpecInsts 中移除 squashed 非推测指令

Squash 原因:
1. 分支预测错误 (branch mispredict)
2. 内存顺序违例 (memory order violation)
3. 异常/中断 (exception/interrupt)
```

## 9. 调试接口

### 9.1 调试输出

```cpp
// 使用 debug/IQ.hh 中的标志
DPRINTF(IQ, "Adding instruction [sn:%llu] PC %s to the IQ.\n",
        new_inst->seqNum, new_inst->pcState());

DPRINTF(IQ, "Processing FU completion [sn:%llu]\n", inst->seqNum);

DPRINTF(IQ, "Scheduling ready instruction [sn:%llu] PC %s\n",
        issuing_inst->seqNum, issuing_inst->pcState());
```

### 9.2 调试命令

```cpp
// 打印所有指令
void printInsts();

// 转储列表大小
void dumpLists();

// 转储所有指令
void dumpInsts();

// 依赖图转储
dependGraph.dump();
```

## 10. 附录：相关文件清单

| 文件 | 类型 | 描述 |
|------|------|------|
| `src/cpu/o3/inst_queue.hh` | 头文件 | IQ 类定义 |
| `src/cpu/o3/inst_queue.cc` | 实现 | IQ 实现 |
| `src/cpu/o3/iew.hh` | 头文件 | IEW 阶段定义 |
| `src/cpu/o3/iew.cc` | 实现 | IEW 阶段实现 |
| `src/cpu/o3/dep_graph.hh` | 模板 | 依赖图实现 |
| `src/cpu/o3/fu_pool.hh` | 头文件 | FU 池定义 |
| `src/cpu/o3/fu_pool.cc` | 实现 | FU 池实现 |
| `src/cpu/o3/mem_dep_unit.hh` | 头文件 | 内存依赖单元定义 |
| `src/cpu/o3/mem_dep_unit.cc` | 实现 | 内存依赖单元实现 |
| `src/cpu/o3/comm.hh` | 头文件 | 模块间通信结构定义 |
| `src/cpu/o3/limits.hh` | 头文件 | 系统限制常量定义 |
| `src/cpu/o3/IQUnit.py` | Python | IQUnit SimObject 定义 |
| `src/cpu/o3/FUPool.py` | Python | FUPool SimObject 定义 |

---

## 版本历史

| 版本 | 日期 | 作者 | 变更描述 |
|------|------|------|----------|
| 1.0 | 2026-04-08 | zhangxi | 初始版本，基于 gem5 v25.1.0.0 |
