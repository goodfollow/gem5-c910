# Fetch Stage 规格说明

## 1. 概述

Fetch（取指）阶段是 O3 CPU 流水线的前端，负责从指令缓存（I-cache）中取指，将原始字节流解码为静态指令（StaticInst），创建动态指令对象（DynInst），并传递给 Decode 阶段。Fetch 阶段与 BAC（Branch and Address Calculation）和 FTQ（Fetch Target Queue）紧密协作，支持两种工作模式：

- **耦合前端（Coupled Frontend）**：BAC 与 Fetch 同步运行，每条指令预译码时进行分支预测
- **解耦前端（Decoupled Frontend）**：BAC 独立于 Fetch 运行，预先生成取指目标并插入 FTQ

## 2. 类接口

### 2.1 `class Fetch`

**头文件**: `src/cpu/o3/fetch.hh`

#### 构造函数
```cpp
Fetch(CPU *_cpu, const BaseO3CPUParams &params);
```

#### 核心方法

| 方法 | 说明 |
|------|------|
| `void tick()` | 每周期主入口；先检查信号更新状态，再执行取指 |
| `void startupStage()` | 初始化阶段，从线程上下文设置初始 PC |
| `void clearStates(ThreadID tid)` | 清除指定线程的全部 Fetch 状态 |
| `void drain()` / `void drainResume()` | 排空/恢复机制，用于 CPU 上下文切换 |
| `bool isDrained() const` | 检查 Fetch 流水线是否为空 |
| `void takeOverFrom()` | 从另一个 CPU 的线程状态接管 |
| `void deactivateThread(ThreadID tid)` | 将线程从取指列表中移除 |
| `void suspendThread(ThreadID tid)` / `void resumeThread(ThreadID tid)` | 线程电源管理 |
| `void setTimeBuffer(TimeBuffer<TimeStruct> *tb_ptr)` | 设置反向通信时间缓冲 |
| `void setDecodeQueue(TimeBuffer<DecodeStruct> *dq_ptr)` | 设置到 Decode 的前向队列 |
| `void setFetchQueue(TimeBuffer<FetchStruct> *fq_ptr)` | 设置内部取指队列 |

#### 内部类

| 类 | 说明 |
|----|------|
| `IcachePort` | 继承 `RequestPort`，处理 I-cache 请求和响应 |
| `FetchTranslation` | 继承 `BaseMMU::Translation`，处理 ITLB 地址翻译 |
| `FinishTranslationEvent` | 延迟翻译完成事件 |

## 3. 数据结构

### 3.1 线程状态枚举（13 种状态）

| 状态 | 触发条件 | 后续转移 |
|------|---------|---------|
| `Running` | 默认状态 | → Idle、Squashing、Blocked、Fetching 等 |
| `Idle` | 无取指需求 | → Running、FtqWait |
| `Squashing` | 收到 squash 信号 | → Running（当 squash 完成） |
| `Blocked` | Decode 反压（下游队列满） | → Running（当解阻塞） |
| `Fetching` | 发起 I-cache 访问 | → ItlbWait、IcacheWaitResponse、IcacheWaitRetry |
| `TrapPending` | 等待 trap 处理 | → Squashing |
| `ItlbWait` | 等待 ITLB 地址翻译 | → IcacheWaitResponse（翻译成功） |
| `IcacheWaitResponse` | 等待 I-cache 响应 | → IcacheAccessComplete |
| `IcacheWaitRetry` | I-cache 繁忙，等待重试 | → IcacheWaitResponse |
| `IcacheAccessComplete` | I-cache 数据已就绪 | → Running |
| `FtqWait` | 解耦模式下 FTQ 为空 | → Running（当 FTQ 就绪） |
| `NoGoodAddr` | 无法解析有效取指地址 | → Squashing |
| `QuiescePending` | 等待静默（quiesce） | → Squashing |

### 3.2 关键成员变量

```cpp
// 每线程状态
ThreadStatus fetchStatus[MaxThreads];
std::unique_ptr<PCStateBase> pc[MaxThreads];      // 当前取指 PC
Addr fetchOffset[MaxThreads];                      // fetchBuffer 内偏移
StaticInstPtr macroop[MaxThreads];                 // 当前宏操作上下文

// 取指缓冲区（每线程一个 cache line 缓冲）
uint8_t *fetchBuffer[MaxThreads];
Addr fetchBufferPC[MaxThreads];
bool fetchBufferValid[MaxThreads];
unsigned fetchBufferSize;
Addr fetchBufferMask;

// 发送到 Decode 前的指令队列
std::deque<DynInstPtr> fetchQueue[MaxThreads];
unsigned fetchQueueSize;

// I-cache 请求跟踪
RequestPtr memReq[MaxThreads];                     // 未完成的内存请求

// 译码器（每线程一个）
InstDecoder *decoder[MaxThreads];

// SMT
SMTFetchPolicy fetchPolicy;
std::list<ThreadID> priorityList;
ThreadID numFetchingThreads;

// BAC 和 FTQ（解耦前端）
BAC *bac;
FTQ *ftq;
```

## 4. Tick 行为

### 4.1 主流程

```
tick()
  └─ 遍历每个活跃线程:
       ├─ checkSignalsAndUpdate(tid)   // 检查 squash、反压、中断
       └─ fetch(tid)                    // 执行取指
  └─ 如果 fetchQueue 不为空:
       └─ 轮询分发指令到 Decode 时间缓冲
  └─ updateFetchStatus()
```

### 4.2 `fetch()` 核心逻辑

```
while (numInst < fetchWidth):
  ├─ 如果解耦前端模式:
  │    └─ 从 FTQ 读取取指目标 (ftq->readHead)
  │       └─ FTQ 为空或未就绪 → break
  │
  ├─ 如果有未完成的内存请求 (memReq[tid]):
  │    └─ 如果支持流水线取指 → pipelineIcacheAccesses(tid)
  │    └─ break
  │
  ├─ 如果 fetchBuffer 无效:
  │    └─ fetchCacheLine(pc, tid) → 发起 ITLB 翻译
  │       └─ 失败 → break
  │
  ├─ decoder->decodeInst() 预译码指令
  │
  ├─ 如果译码成功:
  │    ├─ buildInst() 创建 DynInst
  │    ├─ 更新 PC (BAC->updatePC 或 inst->advancePC)
  │    ├─ 推入 fetchQueue[tid]
  │    ├─ 更新 fetchOffset
  │    └─ 如果遇到 macroop  continuation，记录 macroop
  │
  └─ 如果 fetchBuffer 耗尽:
       └─ fetchBufferValid = false
```

### 4.3 指令分发到 Decode

在 `tick()` **末尾**，fetchQueue 中的指令以**轮询（round-robin）**方式跨线程分发到 Decode：

```cpp
// 优先级列表轮询，每条指令分配到一个 toDecode->fetchInfo 槽位
// 最终设置 toDecode->fetchInfo[tid].size = 实际分发数量
```

## 5. I-Cache 访问机制

### 5.1 `fetchCacheLine(Addr vaddr, ThreadID tid, Addr pc)`

1. 创建 `Request`，标记为 `Request::INST_FETCH`
2. 创建 `FetchTranslation` 对象
3. 调用 `cpu->mmu->translateTiming()` 发起 ITLB 地址翻译
4. 翻译成功返回 `true`，失败返回 `false`

### 5.2 `FetchTranslation` 翻译完成处理

继承 `BaseMMU::Translation`，`finish()` 方法：
- **翻译失败**：创建携带 fault 的 nop 指令，插入 fetchQueue
- **翻译成功**：通过 `icachePort` 发送 I-cache 请求

### 5.3 `IcachePort` 端口

继承 `RequestPort`：
- `recvTimingResp(PacketPtr)`: 接收 I-cache 响应
  - 将数据复制到 `fetchBuffer[tid]`
  - 设置 `fetchBufferValid[tid] = true`
  - 状态转移为 `IcacheAccessComplete`
- `recvReqRetry()`: I-cache 繁忙时收到重试信号
  - 从 `IcacheWaitRetry` 转移到 `IcacheWaitResponse`

### 5.4 翻译流程

```
fetchCacheLine()
  └─ cpu->mmu->translateTiming()
       ├─ ITLB 命中 → 立即 finish()
       │   └─ icachePort.sendTimingReq() → I-cache
       │       ├─ 命中 → recvTimingResp() → fetchBuffer 填充数据
       │       └─ 未命中 → I-cache miss 处理
       └─ ITLB 未命中 → 页表遍历 → 延迟 finish()
```

## 6. SMT 取指策略

| 策略 | 选择方法 |
|------|---------|
| `RoundRobin` | 按优先级列表轮询 |
| `IQCount` | 空闲 IQ 条目最多的线程优先 |
| `LSQCount` | 空闲 LSQ 条目最多的线程优先 |
| `BranchCount` | 未预测分支数最少的线程优先 |

**RoundRobin 实现：**
```cpp
ThreadID roundRobin() {
    for (auto it = priorityList.begin(); it != priorityList.end(); ++it) {
        if (fetchStatus[*it] == Running) {
            ThreadID tid = *it;
            priorityList.erase(it);
            priorityList.push_back(tid);
            return tid;
        }
    }
    return InvalidThreadID;
}
```

## 7. Squash 处理

### 7.1 Squash 来源

| 来源 | 条件 | 处理动作 |
|------|------|---------|
| Commit | `fromCommit->commitInfo[tid].squash` | 清空 fetchBuffer，PC 重定向到 nextPC |
| IEW | `fromIEW->iewInfo[tid].squash` | 同 Commit |
| Decode | 分支预测验证失败 | 重定向到 nextPC |
| BAC | 内部重定向 | 使用 mispredictInst 更新 BTB |

### 7.2 Squash 处理流程

```cpp
// 在 checkSignalsAndUpdate() 中:
收到 squash 信号:
  ├─ 清空 fetchQueue[tid]
  ├─ 设置 fetchBufferValid[tid] = false
  ├─ 更新 PC = squashInfo->nextPC
  ├─ 如果提供了 mispredictInst → bac->updateBTB()
  └─ 设置 fetchStatus[tid] = Squashing
```

## 8. 配置参数

| 参数 | 类型 | 说明 |
|------|------|------|
| `fetchWidth` | unsigned | 每周期最大取指数量 |
| `fetchQueueSize` | unsigned | 取指队列深度 |
| `fetchBufferSize` | unsigned | 取指缓冲区大小（字节） |
| `decodeToFetchDelay` | Cycles | Decode 反传到 Fetch 的延迟 |
| `renameToFetchDelay` | Cycles | Rename 反传到 Fetch 的延迟 |
| `iewToFetchDelay` | Cycles | IEW 反传到 Fetch 的延迟 |
| `commitToFetchDelay` | Cycles | Commit 反传到 Fetch 的延迟 |
| `numThreads` | ThreadID | SMT 线程数 |
| `decoupledFrontEnd` | bool | 是否启用解耦前端（FTQ 模式） |
| `fetchTargetWidth` | unsigned | 解耦模式下每次搜索的字节数 |
| `icachePort` | MasterPort | I-cache 端口 |

## 9. 统计信息

| 统计项 | 类型 | 说明 |
|--------|------|------|
| `status` | Vector | 各 Fetch 状态消耗的周期数 |
| `predictedBranches` | Scalar | 预测的分支数量 |
| `squashes` | Scalar | Fetch 阶段的 squash 次数 |
| `icacheStallCycles` | Scalar | 等待 I-cache 的停顿周期 |
| `tlbStallCycles` | Scalar | 等待 ITLB 的停顿周期 |
| `cacheLines` | Scalar | 获取的 cache line 数量 |
| `branchMispredicts` | Scalar | 分支预测错误次数 |
| `nisnDist` | Distribution | squash 之间指令数量的分布 |

## 10. Probe 探针点

| 探针 | 负载类型 | 触发条件 |
|------|---------|---------|
| `Fetch` | `DynInstPtr` | 指令被取指 |
| `Squash` | `DynInstPtr` | Fetch 阶段被 squash |

## 11. 阶段间通信

### 11.1 前向：Fetch → Decode

```cpp
struct FetchStruct {
    int size;
    DynInstPtr insts[MaxWidth];
    Fault fetchFault;
    InstSeqNum fetchFaultSN;
    bool clearFetchFault;
};
```

通过 `toDecode->fetchInfo[tid]` 经由 `fetchQueue` TimeBuffer 传递。

### 11.2 反向：Decode/Commit/IEW → Fetch

通过 `TimeStruct::CommitComm`：
- `squash`：Squash 信号（来自 Commit/Decode/IEW）
- `nextPC`：重定向 PC 地址
- `mispredictInst`：预测错误的指令
- `squashInst`：导致非预测错误 squash 的指令
- `branchTaken`：分支方向
- `interruptPending`：中断挂起通知
- `trapPending`：Trap 挂起通知

通过 `TimeStruct::DecodeComm`：
- Decode 分支验证产生的 squash 信号

## 12. 关键设计要点

1. **Fetch 不直接解码为 DynInst** —— 通过 `decoder->decodeInst()` 创建 `StaticInst`，再通过 `buildInst()` 包装为 `DynInst`。

2. **指令批量分发** —— Fetch 在 tick 期间将指令累积到 `fetchQueue[tid]`，然后在 tick **末尾**以轮询方式分发到 Decode。

3. **解耦前端模式** —— 启用 `decoupledFrontEnd` 时，BAC 独立运行并预先生成取指目标到 FTQ 中。Fetch 从 FTQ 读取目标而非自行计算。

4. **Fetch Buffer 优化** —— 单缓存行缓冲区（`fetchBuffer`）避免了对同一 cache line 内顺序指令的重复 I-cache 访问。

5. **流水线 I-cache** —— 当 `issuePipelinedIfetch` 启用时，Fetch 可以在等待前一个 I-cache 请求完成的同时发起新的请求。

6. **Fault 携带 nop** —— 当取指过程中发生 fault（如页错误），Fetch 会创建一个携带 fault 的 nop 指令，确保错误能正确传递到 Commit 阶段处理。
