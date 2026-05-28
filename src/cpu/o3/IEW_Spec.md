# IEW Stage 规格说明

## 1. 概述

IEW（Issue/Execute/Writeback）阶段是 O3 CPU 的核心执行引擎，包含三个子阶段：

1. **Issue（发射）**：将指令从 Rename 分发到 IQ（Instruction Queue）和 LSQ（Load-Store Queue），IQ 根据数据依赖关系调度就绪指令
2. **Execute（执行）**：执行指令，包括非内存指令通过 FUPool 执行，内存指令通过 LSQ 执行
3. **Writeback（写回）**：将执行结果写回，唤醒依赖指令，更新 Scoreboard

IEW 阶段还负责分支预测错误检测、内存序违例检测和报告。

## 2. 类接口

### 2.1 `class IEW`

**头文件**: `src/cpu/o3/iew.hh`

#### 构造函数
```cpp
IEW(CPU *_cpu, const BaseO3CPUParams &params);
```

#### 核心方法

| 方法 | 说明 |
|------|------|
| `void tick()` | 每周期主入口；按序执行 dispatch、execute、writeback |
| `void startupStage()` | 初始化阶段，广播 IQ/LSQ 空闲条目 |
| `void clearStates(ThreadID tid)` | 清除指定线程的全部 IEW 状态 |
| `void drain()` / `void drainResume()` | 排空/恢复机制 |
| `void takeOverFrom()` | 从另一个 CPU 的线程状态接管 |
| `void deactivateThread(ThreadID tid)` | 将线程从活跃列表移除 |
| `bool hasStoresToWB(ThreadID tid)` | 是否有待写回的 store |
| `void squash(ThreadID tid, InstSeqNum squash_seq_num)` | Squash 处理 |
| `void setTimeBuffer(TimeBuffer<TimeStruct> *tb_ptr)` | 设置反向通信时间缓冲 |
| `void setRenameQueue(TimeBuffer<RenameStruct> *rq_ptr)` | 设置来自 Rename 的队列 |
| `void setIEWQueue(TimeBuffer<IEWStruct> *iq_ptr)` | 设置到 Commit 的前向队列 |
| `void setScoreboard(Scoreboard *sb_ptr)` | 设置 Scoreboard 指针 |
| `void setFUPool(FUPool *fu_pool_ptr)` | 设置功能单元池 |

## 3. 数据结构

### 3.1 状态枚举

**Status（总体状态）：**
| 状态 | 说明 |
|------|------|
| `Active` | 阶段活跃 |
| `Inactive` | 阶段不活跃 |

**StageStatus（子阶段状态）：**
| 状态 | 说明 |
|------|------|
| `Running` | 正常运行 |
| `Idle` | 空闲 |
| `Squashing` | 正在 squash |
| `Blocked` | 被阻塞 |
| `Unblocking` | 正在解阻塞 |

**ThreadStatus（每线程状态）：**
| 状态 | 说明 |
|------|------|
| `Running` | 正常执行 |
| `Idle` | 空闲 |
| `Squashing` | 正在 squash |
| `Blocked` | 被阻塞 |
| `Unblocking` | 正在解阻塞 |

### 3.2 关键成员变量

```cpp
Status _status;
StageStatus dispatchStatus, execStatus, wbStatus;
ThreadStatus threadStatus[MaxThreads];

// 内部 TimeBuffer（dispatch → execute → writeback）
TimeBuffer<IssueStruct> issueToExecQueue;

// 指令队列和 LSQ
std::shared_ptr<IQ> iq;                    // 指令队列
std::shared_ptr<LSQ> ldstQueue;            // Load-Store Queue

// 功能单元池
FUPool *fuPool;

// 资源跟踪
std::vector<int> numROBFreeEntries;
std::vector<bool> robSquashing;

// 写回槽位
int wbNumInst;                             // 本周期写回槽位数
Cycles wbCycle;                            // 写回周期计数

// Squash 相关
bool includeSquashInst[MaxThreads];         // squash 是否包含触发指令
InstSeqNum squashSeqNum[MaxThreads];

// 停顿源
struct Stalls {
    bool commit;                            // Commit 反压
    bool lqFull;                            // Load Queue 满
    bool sqFull;                            // Store Queue 满
} stalls[MaxThreads];

// Serialize 相关
DynInstPtr serializeInst[MaxThreads];       // 当前序列化指令

// SMT 相关
ThreadID numThreads;
std::list<ThreadID> *activeThreads;

// 配置参数
const unsigned numIssued;                   // 每周期最大发射宽度
const unsigned decodeToIEWDelay;            // Decode 到 IEW 的延迟
const Cycles renameToIEWDelay;              // Rename 到 IEW 的延迟
const Cycles commitToIEWDelay;              // Commit 反传到 IEW 的延迟
const bool storeWritingEnable;              // 是否启用 store 写回
const bool iqSingleCycle;                   // IQ 是否单周期发射
const bool broadcast_free_entries;          // 是否广播空闲条目
```

## 4. Tick 行为

### 4.1 主流程（严格按序执行）

```
tick()
  │
  ├─ 1. ldstQueue.tick()                   // LSQ 处理内存操作
  │
  ├─ 2. sortInsts()                        // 按类型分离内存/非内存指令
  │
  ├─ 3. processFreeUnits()                 // 处理空闲功能单元
  │
  ├─ 4. checkSignalsAndUpdate()            // 检查 squash、反压信号
  │   └─ 处理 Commit 的反压和 squash 信号
  │
  ├─ 5. dispatchInsts()                    // Dispatch 子阶段
  │   └─ 将指令分发到 IQ/LSQ
  │
  ├─ 6. executeInsts()                     // Execute 子阶段
  │   ├─ 检查分支预测错误
  │   ├─ 检查内存序违例
  │   └─ 执行非内存指令
  │
  ├─ 7. writebackInsts()                   // Writeback 子阶段
  │   ├─ 唤醒依赖指令
  │   ├─ 更新 Scoreboard
  │   └─ 检查 pinned 寄存器写回完成
  │
  ├─ 8. scheduleReadyInsts()               // 调度就绪指令
  │   └─ IQ 检查依赖并调度可执行指令
  │
  ├─ 9. issueToExecQueue.advance()         // 推进内部时间缓冲
  │
  ├─ 10. writebackStores()                 // 写回 store 指令
  │
  └─ 11. 处理 Commit 更新
       └─ 接收 nonSpecSeqNum，处理非推测指令
```

## 5. Dispatch 子阶段

### 5.1 `dispatchInsts()` - 指令分发

```
dispatchInsts(ThreadID tid)
  └─ 遍历从 Rename 收到的每条指令:
       └─ 根据指令类型路由:
            ├─ Load → LSQ.insertLoad(inst)
            │   └─ 失败 (LSQ 满) → 设置 Blocked, stalls.lqFull
            │
            ├─ Store → LSQ.insertStore(inst)
            │   └─ 失败 (LSQ 满) → 设置 Blocked, stalls.sqFull
            │
            ├─ Atomic → LSQ.insertStore(inst) + IQ.insertNonSpec(inst)
            │   └─ 原子操作同时进入 LSQ 和 IQ（非推测）
            │
            ├─ StoreConditional → IQ.insertNonSpec(inst)
            │   └─ 条件 store 作为非推测指令处理
            │
            ├─ Barrier → IQ.insertBarrier(inst)
            │   └─ Barrier 指令特殊处理
            │
            └─ 普通指令 → IQ.insert(inst)
                └─ 失败 (IQ 满) → 设置 Blocked
```

### 5.2 Dispatch 停顿条件

| 条件 | 标志 | 处理 |
|------|------|------|
| Commit 反压 | `stalls.commit` | 停止 dispatch |
| IQ 满 | `stalls.lqFull` / `stalls.sqFull` | 停止 dispatch |
| Serialize 阻塞 | `serializeInst[tid] != nullptr` | 等待序列化指令完成 |

## 6. IQ（Instruction Queue）

### 6.1 IQ 功能

| 功能 | 说明 |
|------|------|
| 依赖跟踪 | 跟踪每条指令的源寄存器依赖关系 |
| 就绪调度 | 当所有源寄存器就绪时，调度指令到功能单元 |
| 功能单元分配 | 根据指令操作类型（OpClass）分配可用的 FU |
| 唤醒依赖 | 指令执行完成后，唤醒等待该结果的依赖指令 |

### 6.2 指令调度

```
scheduleReadyInsts()
  └─ 遍历 IQ 中的每条指令:
       ├─ 检查所有源寄存器是否就绪
       │   └─ 未就绪 → 跳过
       │
       ├─ 检查序列化阻塞
       │   └─ 如果有 serialize 指令未完成 → 跳过
       │
       ├─ 检查非推测指令
       │   └─ 等待 commit 广播的 nonSpecSeqNum
       │
       ├─ 查找可用的功能单元
       │   └─ 遍历 FUPool，匹配 opClass
       │   └─ 无可用 FU → 跳过
       │
       ├─ 分配功能单元并执行
       │   ├─ 设置指令已发射 (setIssued)
       │   ├─ 记录延迟 (latency)
       │   └─ 将指令放入 issueToExecQueue，延迟后执行
       │
       └─ 唤醒依赖指令 (wakeDependents)
            └─ 遍历 IQ 中所有等待该结果的指令
               └─ 标记源寄存器就绪
```

### 6.3 内部 TimeBuffer

IQ 使用内部 `TimeBuffer<IssueStruct>` 来模拟功能单元的延迟：

```cpp
TimeBuffer<IssueStruct> issueToExecQueue;  // dispatch → execute
```

指令发射后，根据 FU 延迟在 `issueToExecQueue` 中设置条目，延迟到期后在 execute 阶段执行。

## 7. Execute 子阶段

### 7.1 `executeInsts()` - 指令执行

```
executeInsts(ThreadID tid)
  └─ 从 issueToExecQueue 获取到期的指令:
       ├─ 检查分支预测错误:
       │    if (inst->isControl() && inst->mispredicted()):
       │       └─ 记录分支预测错误
       │          └─ toCommit->iewInfo[tid].mispredictInst = inst
       │
       ├─ 检查内存序违例:
       │    if (ldstQueue.violation(tid)):
       │       └─ 记录内存序违例
       │          └─ 触发 squash
       │
       └─ 执行非内存指令:
            └─ 调用 inst->execute()
               └─ 设置指令已执行 (setExecuted)
```

### 7.2 分支预测检测

```cpp
bool mispredicted() const {
    std::unique_ptr<PCStateBase> next_pc(pc->clone());
    staticInst->advancePC(*next_pc);    // 计算实际 PC
    return *next_pc != *predPC;         // 与预测 PC 比较
}
```

- 当控制指令执行后，比较实际计算的 PC 和预测的 PC
- 不匹配则设置 `mispredictInst`，报告给 Commit 阶段

### 7.3 内存序违例检测

```cpp
// LSQ 检测到有 younger load 读取了 older store 未写入的数据
if (ldstQueue.violation(tid)) {
    DPRINTF(IEW, "[tid:%i] Memory order violation detected\n", tid);
    includeSquashInst[tid] = true;      // 包含违例指令在 squash 中
    toCommit->iewInfo[tid].squash = true;
    toCommit->iewInfo[tid].squashedSeqNum = violator->seqNum;
}
```

## 8. Writeback 子阶段

### 8.1 `writebackInsts()` - 结果写回

```
writebackInsts(ThreadID tid)
  └─ 遍历本周期完成的指令:
       ├─ 唤醒依赖指令 (wakeDependents)
       │   └─ 遍历 IQ 中等待该指令结果的指令
       │      └─ 标记源寄存器就绪 (markSrcRegReady)
       │
       ├─ 更新 Scoreboard
       │   └─ scoreboard->setRegReady(dest_phys_reg)
       │
       ├─ 检查 pinned 寄存器写回完成
       │   └─ if (getNumPinnedWritesToComplete() == 0):
       │       └─ 指令可以写回
       │
       └─ 推送到 toCommit->iewInfo[tid].insts[]
```

### 8.2 写回带宽

```cpp
wbNumInst = 写回槽位数;
wbCycle = 写回周期;

// 当写回指令超过带宽时:
if (writeback_count > wbNumInst) {
    wbNumInst += numIssued;     // 增加下周写回槽位
    wbCycle++;
}
```

### 8.3 Store 写回

```cpp
writebackStores()
  └─ 遍历已完成的 store 指令:
       └─ 如果 storeWritingEnable:
            ├─ 将 store 数据写入缓存
            └─ 设置指令已执行
```

## 9. LSQ（Load-Store Queue）

### 9.1 LSQ 功能

| 功能 | 说明 |
|------|------|
| Load 插入 | 将 load 指令插入 LSQ，跟踪内存依赖 |
| Store 插入 | 将 store 指令插入 LSQ，缓冲 store 数据 |
| Load 执行 | 执行 load，从 store buffer 或缓存获取数据 |
| Store 提交 | 在 Commit 确认后将 store 数据写入内存 |
| 内存序检查 | 检测 load-load、load-store、store-load 的序违例 |
| 地址翻译 | 处理 DTLB 地址翻译和页表遍历 |

### 9.2 Load 执行

```
LSQUnit::executeLoad(inst)
  ├─ 如果地址翻译未完成:
  │    └─ 发起 DTLB 翻译
  │
  ├─ 检查 store buffer 中是否有匹配的 older store:
  │    ├─ 有 → 从 store buffer 转发数据 (load forwarding)
  │    └─ 无 → 从缓存读取数据
  │
  ├─ 检查内存序违例:
  │    └─ 遍历 younger loads，检查是否有乱序依赖
  │
  └─ 设置 load 完成
```

## 10. Squash 处理

### 10.1 Squash 来源

| 来源 | 条件 | 处理 |
|------|------|------|
| **Commit** | 分支预测错误 | squash IQ 和 LSQ 中错误路径指令 |
| **Commit** | 内存序违例 | squash 违例指令及之后的指令 |
| **Commit** | Trap | squash 所有指令，等待 trap 处理 |
| **自身** | 内存序违例检测 | 报告给 Commit，由 Commit 发起 squash |

### 10.2 Squash 流程

```cpp
void IEW::squash(ThreadID tid, InstSeqNum squash_seq_num)
{
    threadStatus[tid] = Squashing;
    squashSeqNum[tid] = squash_seq_num;

    // IQ squash
    iq->squash(squash_seq_num, tid);

    // LSQ squash
    ldstQueue.squash(squash_seq_num, tid);

    // 重置 serialize 状态
    serializeInst[tid] = nullptr;

    // 重置 stall 状态
    stalls[tid].commit = false;
    stalls[tid].lqFull = false;
    stalls[tid].sqFull = false;
}
```

## 11. 非推测指令处理

### 11.1 Commit 广播 nonSpecSeqNum

某些指令（如 barrier、store conditional）必须在 ROB 头部才能执行：

```cpp
// Commit 阶段：
if (head_inst->isNonSpeculative()) {
    toIEW->commitInfo[tid].nonSpecSeqNum = head_inst->seqNum;
}

// IEW 阶段：
if (fromCommit->commitInfo[tid].nonSpecSeqNum != 0) {
    // IQ 可以调度该非推测指令
    iq->scheduleNonSpec(fromCommit->commitInfo[tid].nonSpecSeqNum);
}
```

### 11.2 严格有序 Load

```cpp
// Commit 检测到严格有序 load 到达 ROB 头部:
if (head_inst->isLoad() && head_inst->strictlyOrdered()) {
    toIEW->commitInfo[tid].strictlyOrdered = true;
    toIEW->commitInfo[tid].strictlyOrderedLoad = head_inst;
}

// IEW 执行该 load:
ldstQueue.executeStrictlyOrderedLoad(strictlyOrderedLoad);
```

## 12. HTM（硬件事务内存）跟踪

```cpp
// 设置 HTM 事务状态:
inst->setHtmTransactionalState(htm_uid, htm_depth);

// 清除 HTM 事务状态:
inst->clearHtmTransactionalState();

// Commit 阶段更新最后退休的 HTM UID:
iewStage->setLastRetiredHtmUid(tid, inst->getHtmTransactionUid());
```

## 13. 配置参数

| 参数 | 类型 | 说明 |
|------|------|------|
| `numIssued` | unsigned | 每周期最大发射指令数 |
| `iqEntries` | unsigned | IQ 总条目数 |
| `lsqEntries` | unsigned | LSQ 总条目数 |
| `decodeToIEWDelay` | Cycles | Decode 到 IEW 的延迟 |
| `renameToIEWDelay` | Cycles | Rename 到 IEW 的延迟 |
| `commitToIEWDelay` | Cycles | Commit 反传到 IEW 的延迟 |
| `storeWritingEnable` | bool | 是否启用 store 写回 |
| `iqSingleCycle` | bool | IQ 是否单周期发射 |
| `broadcast_free_entries` | bool | 是否广播 IQ/LSQ 空闲条目 |
| `wbWidth` | unsigned | 写回宽度 |
| `numThreads` | ThreadID | SMT 线程数 |

## 14. 统计信息

| 统计项 | 类型 | 说明 |
|--------|------|------|
| `status` | Vector | 各 IEW 状态消耗的周期数 |
| `dispatched` | Scalar | 已分发的指令数 |
| `issued` | Scalar | 已发射的指令数 |
| `executed` | Scalar | 已执行的指令数 |
| `wbRate` | Scalar | 写回速率 |
| `wbFanout` | Scalar | 每次写回唤醒的依赖指令数 |
| `branchMispredicts` | Scalar | 分支预测错误次数 |
| `squashedInsts` | Scalar | IEW 阶段被 squash 的指令数 |
| `dispatchStall` | Scalar | Dispatch 停顿次数 |

## 15. Probe 探针点

| 探针 | 负载类型 | 触发条件 |
|------|---------|---------|
| `IEW` | `DynInstPtr` | 指令到达 IEW |
| `Mispredict` | `DynInstPtr` | 检测到分支预测错误 |
| `Dispatch` | `DynInstPtr` | 指令被分发到 IQ/LSQ |
| `Execute` | `DynInstPtr` | 指令被执行 |
| `ToCommit` | `DynInstPtr` | 指令发送到 Commit |
| `Squash` | `DynInstPtr` | IEW 阶段被 squash |

## 16. 阶段间通信

### 16.1 输入：来自 Rename

通过 `renameQueue` TimeBuffer 接收：

```cpp
struct RenameStruct {
    int size;
    DynInstPtr insts[MaxWidth];
};
```

### 16.2 输出：到 Commit

通过 `iewQueue` TimeBuffer 发送：

```cpp
struct IEWStruct {
    int size;
    DynInstPtr insts[MaxWidth];
    DynInstPtr mispredictInst[MaxThreads];
    Addr mispredPC[MaxThreads];
    InstSeqNum squashedSeqNum[MaxThreads];
    std::unique_ptr<PCStateBase> pc[MaxThreads];
    bool squash[MaxThreads];
    bool branchMispredict[MaxThreads];
    bool branchTaken[MaxThreads];
    bool includeSquashInst[MaxThreads];
};
```

### 16.3 反向信号：来自 Commit

通过 `TimeStruct::CommitComm` 接收：

| 字段 | 说明 |
|------|------|
| `squash` | Squash 信号 |
| `robSquashing` | ROB 正在 squash |
| `doneSeqNum` | 已提交/已 squash 的序列号 |
| `nonSpecSeqNum` | 非推测指令序列号 |
| `strictlyOrdered` | 严格有序访问标志 |
| `strictlyOrderedLoad` | 严格有序 load 指令 |

### 16.4 广播：到 Fetch/Rename

通过 `TimeStruct::IewComm` 广播 IQ/LSQ 空闲条目：

| 字段 | 说明 |
|------|------|
| `freeIQEntries` | IQ 空闲条目数 |
| `freeLQEntries` | LQ 空闲条目数 |
| `freeSQEntries` | SQ 空闲条目数 |
| `usedIQ` / `usedLSQ` | IQ/LSQ 使用情况 |

## 17. 流水线内部结构

```
┌──────────┐  renameQueue  ┌──────────────────────────────────────┐
│  Rename  │ ────────────▶ │              IEW                     │
│          │               │                                      │
│ - Dispatch:              │  ┌─────────┐   ┌─────────┐          │
│   Route to IQ/LSQ        │  │  IQ     │──▶│ Execute │──▶ WB    │
│                          │  └─────────┘   └─────────┘          │
│                          │       │                              │
│                          │  ┌─────────┐   ┌─────────┐          │
│                          │  │  LSQ    │──▶│  LSQ    │──▶ Stores│
│                          │  └─────────┘   └─────────┘          │
│                          │       │                              │
│                          │  ┌──────────────────────┐           │
│                          │  │   issueToExecQueue   │           │
│                          │  │   (内部 TimeBuffer)  │           │
│                          │  └──────────────────────┘           │
└──────────┘               └──────────────────────────────────────┘
```

## 18. 关键设计要点

1. **IQ 依赖图调度** —— IQ 维护指令间的依赖关系图，当指令的所有源寄存器就绪时自动调度，实现真正的乱序执行。

2. **Back-to-Back 调度** —— IQ 将功能单元延迟与调度延迟合并，指令执行完成的同时唤醒依赖指令，支持背靠背调度（如 ALU 操作的链式依赖可在连续周期执行）。

3. **内存序违例检测** —— LSQ 检测 younger load 读取了 older store 未写入的数据（store-load violation），触发 squash 确保内存序正确性。

4. **Dispatch 到 Execute 分离** —— Dispatch 将指令放入 IQ/LSQ，Execute 从 IQ 调度执行。这种分离允许指令在 IQ 中等待依赖，而不阻塞 Dispatch。

5. **Store Buffer** —— Store 数据先写入 LSQ 的 store buffer，在 Commit 确认后才真正写入内存，确保推测执行的 store 不会影响内存状态。

6. **非推测指令** —— Barrier、store conditional 等指令必须在 ROB 头部才能执行，通过 Commit 广播 `nonSpecSeqNum` 通知 IEW 可以安全执行。
