# Decode Stage 规格说明

## 1. 概述

Decode（译码）阶段位于 Fetch 和 Rename 之间，是 O3 CPU 流水线中的缓冲和验证层。Decode 阶段**不执行复杂的指令译码**（字节到 StaticInst 的转换已在 Fetch 阶段通过 `InstDecoder` 完成），而是负责：

1. 接收 Fetch 传来的动态指令（DynInst）
2. 验证 PC 相对分支预测的正确性
3. 将指令转发给 Rename 阶段
4. 处理 squash 和解阻塞操作

## 2. 类接口

### 2.1 `class Decode`

**头文件**: `src/cpu/o3/decode.hh`

#### 构造函数
```cpp
Decode(CPU *_cpu, const BaseO3CPUParams &params);
```

#### 核心方法

| 方法 | 说明 |
|------|------|
| `void tick()` | 每周期主入口；检查信号并执行译码 |
| `void startupStage()` | 初始化阶段状态 |
| `void clearStates(ThreadID tid)` | 清除指定线程的全部 Decode 状态 |
| `void drain()` / `void drainResume()` | 排空/恢复机制 |
| `void takeOverFrom()` | 从另一个 CPU 的线程状态接管 |
| `void deactivateThread(ThreadID tid)` | 将线程从活跃列表移除 |
| `void setTimeBuffer(TimeBuffer<TimeStruct> *tb_ptr)` | 设置反向通信时间缓冲 |
| `void setFetchQueue(TimeBuffer<FetchStruct> *fq_ptr)` | 设置来自 Fetch 的队列 |
| `void setDecodeQueue(TimeBuffer<DecodeStruct> *dq_ptr)` | 设置到 Rename 的前向队列 |
| `unsigned getInstsAvailable(ThreadID tid)` | 获取当前可用的指令数 |

## 3. 数据结构

### 3.1 状态枚举

**DecodeStatus（总体状态）：**
| 状态 | 说明 |
|------|------|
| `Active` | 阶段活跃，有指令在处理 |
| `Inactive` | 阶段不活跃，可以被 CPU 调度器去激活 |

**ThreadStatus（每线程状态）：**
| 状态 | 说明 | 触发条件 |
|------|------|---------|
| `Running` | 正常译码 | 默认状态 |
| `Idle` | 空闲 | 无指令可处理 |
| `Blocked` | 被阻塞 | Rename 队列满 |
| `Unblocking` | 正在解阻塞 | 下游恢复中 |
| `Squashing` | 正在 squash | 收到 squash 信号 |

### 3.2 停顿源

```cpp
struct Stalls {
    bool rename;   // Rename 阶段反压（下游队列满）
};
Stalls stalls[MaxThreads];
```

Decode 只有一个停顿源：Rename 阶段的反压信号。

### 3.3 关键成员变量

```cpp
DecodeStatus decodeStatus;                       // 总体状态
ThreadStatus decodeStatus[MaxThreads];           // 每线程状态

std::queue<DynInstPtr> insts[MaxThreads];        // 本地指令队列
std::queue<DynInstPtr> skidBuffer[MaxThreads];   // 滑移缓冲区

DynInstPtr squashInst[MaxThreads];               // squash 相关指令

ThreadID numThreads;                             // SMT 线程数
std::list<ThreadID> *activeThreads;              // 活跃线程列表

// 延迟参数
const Cycles renameToDecodeDelay;                // Rename 反传到 Decode 的延迟
const Cycles iewToDecodeDelay;                   // IEW 反传到 Decode 的延迟
const Cycles commitToDecodeDelay;                // Commit 反传到 Decode 的延迟
const Cycles fetchToDecodeDelay;                 // Fetch 到 Decode 的延迟

// 宽度参数
const unsigned decodeWidth;                      // 每周期最大译码宽度
const unsigned skidBufferMax;                    // 滑移缓冲区最大容量
```

## 4. Tick 行为

### 4.1 主流程

```
tick()
  └─ status_change = false
  └─ 遍历每个活跃线程:
       ├─ checkSignalsAndUpdate(tid)    // 检查信号并更新状态
       │   └─ status_change |= 返回值
       └─ decode(status_change, tid)    // 执行译码
  └─ toDecode->advance()               // 推进时间缓冲
  └─ 如果 status_change:
       └─ updateStatus()               // 更新总体状态
```

### 4.2 `checkSignalsAndUpdate()` - 信号处理

Decode 接收的信号来源：

| 来源 | 信号 | 处理动作 |
|------|------|---------|
| **Fetch** | 新指令到达 | 从 fetchQueue 读取指令到 insts 队列 |
| **Rename** | 反压（`renameBlock`） | 设置 Blocked，指令移入 skidBuffer |
| **Rename** | 解阻塞（`renameUnblock`） | 设置 Unblocking |
| **Commit** | squash | 清空 insts 队列，标记 Squashing |
| **IEW** | squash | 清空 insts 队列，标记 Squashing |

**Squash 处理：**
```cpp
if (fromCommit->commitInfo[tid].squash ||
    fromIEW->iewInfo[tid].squash) {

    // 获取 squash 序列号
    InstSeqNum squash_seq_num = squash 的来源序列号;

    // 清空当前指令队列到滑移缓冲区
    while (!insts[tid].empty()) {
        skidBuffer[tid].push(insts[tid].front());
        insts[tid].pop();
    }

    // 找到 squash 指令的位置
    squashInst[tid] = 最后处理的指令;

    // 设置 squash 状态
    decodeStatus[tid] = Squashing;

    // 向 BAC 发送 squash 信号
    toDecode->decodeInfo[tid].squash = true;
    toDecode->decodeInfo[tid].nextPC = new_pc;
    wroteToTimeBuffer = true;
}
```

### 4.3 `decode()` - 译码调度

```cpp
void Decode::decode(bool &status_change, ThreadID tid)
{
    if (decodeStatus[tid] == Running) {
        decodeInsts(tid);                          // 正常译码
    } else if (decodeStatus[tid] == Unblocking) {
        if (skidsEmpty()) {                         // 等待滑移缓冲区清空
            decodeStatus[tid] = Running;
            toDecode->decodeInfo[tid].used = true;
            wroteToTimeBuffer = true;
            status_change = true;
        }
    } else if (decodeStatus[tid] == Squashing) {
        if (squashInst[tid] == nullptr) {           // squash 完成
            decodeStatus[tid] = Running;
            status_change = true;
        }
    }
}
```

## 5. 核心译码逻辑

### 5.1 `decodeInsts()` - 指令处理

```
decodeInsts(ThreadID tid)
  ├─ 获取可用指令数: from_fetch = min(fetchQueue.size, decodeWidth)
  │
  └─ 循环处理每条指令 (i = 0; i < from_fetch):
       ├─ 取出指令: inst = fetchQueue->fromFetch[tid].insts[i]
       │
       ├─ 【验证 1】PC 相对分支预测:
       │    if (inst->isDirectCtrl()):
       │       └─ if (*target != inst->readPredTarg()):
       │            └─ squash(inst, true, tid)  ← 分支预测错误
       │                 └─ return
       │
       ├─ 【验证 2】控制指令类型一致性:
       │    if (inst->readPredTaken() && !inst->isControl()):
       │       └─ panic()  ← 控制类型不匹配
       │
       └─ 添加到本地指令队列: insts[tid].push(inst)
  │
  └─ 将指令转发到 Rename:
       └─ 如果 insts[tid] 不为空:
            ├─ 设置 toRename->decodeInfo[tid].size
            ├─ 逐条复制到 toRename->decodeInfo[tid].insts[]
            ├─ 清空 insts[tid]
            └─ wroteToTimeBuffer = true
```

### 5.2 分支预测验证

#### 直接控制流验证

```cpp
if (inst->isDirectCtrl()) {
    const PCStateBase *target = inst->branchTarget().get();
    if (*target != inst->readPredTarg()) {
        squash(inst, true, tid);  // 分支预测错误
        return;
    }
}
```

- `inst->branchTarget()`: 实际计算的分支目标地址
- `inst->readPredTarg()`: Fetch 阶段预测的分支目标地址
- 不匹配说明 Fetch 阶段的分支预测错误，需要 squash

#### 控制类型验证

```cpp
if (inst->readPredTaken() && !inst->isControl()) {
    panic("Control-type mismatch");  // 控制类型不匹配
}
```

- Fetch 阶段将非控制指令标记为控制指令（预测为 taken）
- 这是严重错误，直接 panic

### 5.3 `squash()` - Squash 处理

```cpp
void Decode::squash(const DynInstPtr &inst, bool branch_mispredict,
                    ThreadID tid)
{
    // 清空队列到滑移缓冲区
    while (!insts[tid].empty()) {
        skidBuffer[tid].push(insts[tid].front());
        insts[tid].pop();
    }

    // 设置 squash 状态
    decodeStatus[tid] = Squashing;
    squashInst[tid] = inst;

    // 向 BAC 发送 squash 信号
    toDecode->decodeInfo[tid].squash = true;
    toDecode->decodeInfo[tid].doneSeqNum = inst->seqNum;
    toDecode->decodeInfo[tid].nextPC = &inst->pcState();
    toDecode->decodeInfo[tid].branchTaken = inst->predTaken();
    toDecode->decodeInfo[tid].branchMispredict = branch_mispredict;
    wroteToTimeBuffer = true;
}
```

## 6. 滑移缓冲区（Skid Buffer）

### 6.1 作用

当 Decode 被 Rename 阻塞时：
1. Fetch 仍在发送指令
2. 这些指令无法立即转发到 Rename
3. 滑移缓冲区临时存储这些指令

### 6.2 操作

```cpp
// 检查滑移缓冲区是否为空
bool skidsEmpty() {
    return skidBuffer[tid].empty();
}

// 解阻塞时处理滑移缓冲区
// 当 decodeStatus 为 Unblocking 且 skidBuffer 清空时:
//   → 状态转回 Running
//   → 向 BAC 发送 used 信号
```

### 6.3 大小计算

```cpp
skidBufferMax = (fetchToDecodeDelay + 1) * decodeWidth;
```

滑移缓冲区大小由 Fetch 到 Decode 的延迟和译码宽度决定，确保能容纳延迟线中所有未处理的指令。

## 7. Squash 处理

### 7.1 Squash 来源

| 来源 | 信号 | 处理方式 |
|------|------|---------|
| **Commit** | `fromCommit->commitInfo[tid].squash` | 清空 insts 到 skidBuffer，设置 Squashing |
| **IEW** | `fromIEW->iewInfo[tid].squash` | 同上 |
| **Decode 自身** | 分支验证失败 | 调用 `squash()` 函数 |

### 7.2 Squash 完成检查

当 `decodeStatus[tid] == Squashing` 时，每周期检查 `squashInst[tid]` 是否为空：
- 为空 → squash 完成，状态转回 `Running`
- 非空 → 继续等待 squash 完成

## 8. 配置参数

| 参数 | 类型 | 说明 |
|------|------|------|
| `decodeWidth` | unsigned | 每周期最大译码指令数 |
| `skidBufferMax` | unsigned | 滑移缓冲区最大容量 |
| `fetchToDecodeDelay` | Cycles | Fetch 到 Decode 的延迟 |
| `renameToDecodeDelay` | Cycles | Rename 反传到 Decode 的延迟 |
| `iewToDecodeDelay` | Cycles | IEW 反传到 Decode 的延迟 |
| `commitToDecodeDelay` | Cycles | Commit 反传到 Decode 的延迟 |
| `numThreads` | ThreadID | SMT 线程数 |

## 9. 统计信息

| 统计项 | 类型 | 说明 |
|--------|------|------|
| `status` | Vector | 各 Decode 状态消耗的周期数 |
| `branchResolved` | Scalar | 在 Decode 阶段解析的分支数量 |
| `branchMispred` | Scalar | Decode 阶段发现的分支预测错误 |
| `controlMispred` | Scalar | Decode 阶段发现的控制类型错误 |
| `decodedInsts` | Scalar | 已译码的指令总数 |
| `squashedInsts` | Scalar | Decode 阶段被 squash 的指令数 |

## 10. 阶段间通信

### 10.1 输入：来自 Fetch

通过 `fetchQueue` TimeBuffer 接收：

```cpp
struct FetchStruct {
    int size;
    DynInstPtr insts[MaxWidth];
};
```

### 10.2 输出：到 Rename

通过 `decodeQueue` TimeBuffer 发送：

```cpp
struct DecodeStruct {
    int size;
    DynInstPtr insts[MaxWidth];
};
```

### 10.3 反向信号：到 BAC/Fetch

通过 `TimeStruct::DecodeComm` 发送：

| 字段 | 说明 |
|------|------|
| `squash` | Squash 标志 |
| `doneSeqNum` | Squash 后的序列号 |
| `nextPC` | 正确的下一条 PC |
| `branchMispredict` | 是否为分支预测错误 |
| `branchTaken` | 分支实际方向 |
| `mispredictInst` | 预测错误的指令 |
| `squashInst` | 导致 squash 的指令 |

### 10.4 反向信号：来自 Rename/Commit/IEW

通过 `TimeStruct::CommitComm` 接收：

| 字段 | 说明 |
|------|------|
| `squash` | 来自 Commit/IEW 的 squash 信号 |
| `robSquashing` | ROB 正在 squash |
| `doneSeqNum` | 已提交/已 squash 的序列号 |

通过 `TimeStruct` 中的阻塞信号接收：
- `renameBlock[tid]`: Rename 反压
- `renameUnblock[tid]`: Rename 解阻塞

## 11. 流水线位置

```
┌──────────┐   fetchQueue   ┌──────────┐  decodeQueue  ┌──────────┐
│  Fetch   │ ─────────────▶ │  Decode  │ ───────────▶  │  Rename  │
│          │                │          │               │          │
│ - I-cache│                │ - 验证分支               │ - 重命名  │
│ - 预译码 │                │ - 转发指令               │ - 分配物理│
│ - DynInst│                │ - squash 处理            │   寄存器  │
└──────────┘                └──────────┘               └──────────┘
```

## 12. 关键设计要点

1. **Decode 为何简单？** —— 在 O3 CPU 中，指令译码（字节到 StaticInst）已在 Fetch 阶段由 `InstDecoder` 完成。Decode 主要作为 Fetch 和 Rename 之间的缓冲，并提供额外的分支预测验证点。

2. **多周期延迟** —— 各阶段间的通信延迟参数（如 `fetchToDecodeDelay`）模拟了指令在流水线阶段之间传输的物理延迟。这些延迟通过 `TimeBuffer` 实现。

3. **滑移缓冲区** —— 当 Rename 队列满时，Decode 无法转发指令。此时 Fetch 继续发送的指令暂存于滑移缓冲区，等待解阻塞后处理。

4. **Decode 是唯一能主动发起 squash 的阶段** —— 除了接收来自 Commit/IEW 的 squash 信号外，Decode 自身在验证 PC 相对分支预测时发现错误，也会主动发起 squash。
