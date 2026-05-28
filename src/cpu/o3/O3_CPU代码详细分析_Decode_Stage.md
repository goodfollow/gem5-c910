# O3 CPU - Decode Stage 详细分析

## 概述

Decode（译码）阶段位于 Fetch 和 Rename 之间，主要负责：
1. 接收 Fetch 传来的动态指令
2. 验证分支预测的正确性（PC 相对分支）
3. 将指令批量转发给 Rename 阶段
4. 处理 squash 和解阻塞操作

Decode 阶段不执行复杂的指令译码（这已在 Fetch 阶段完成），而是作为 Fetch 和 Rename 之间的缓冲和验证层。

---

## 一、Decode 阶段架构

### 1.1 类定义 (decode.hh)

```cpp
class Decode
{
  public:
    enum DecodeStatus {
        Active,     // 阶段活跃
        Inactive    // 阶段不活跃
    };

    enum ThreadStatus {
        Running,       // 正常译码
        Idle,          // 空闲
        StartSquash,   // 开始 squash
        Squashing,     // 正在 squash
        Blocked,       // 被阻塞
        Unblocking,    // 正在解阻塞
        ThreadStatusMax
    };

  private:
    DecodeStatus _status;
    ThreadStatus decodeStatus[MaxThreads];

    // 指令队列
    std::queue<DynInstPtr> insts[MaxThreads];

    // 滑移缓冲区
    std::queue<DynInstPtr> skidBuffer[MaxThreads];

    // 停顿源
    struct Stalls {
        bool rename;   // Rename 阶段反压
    };
    Stalls stalls[MaxThreads];

    // Squash 相关
    DynInstPtr squashInst[MaxThreads];

    // SMT 相关
    ThreadID numThreads;
    std::list<ThreadID> *activeThreads;

    // 配置参数
    const Cycles renameToDecodeDelay;
    const Cycles iewToDecodeDelay;
    const Cycles commitToDecodeDelay;
    const Cycles fetchToDecodeDelay;
    const unsigned decodeWidth;
    const unsigned skidBufferMax;
};
```

### 1.2 线程状态说明

| 状态 | 说明 | 触发条件 |
|------|------|---------|
| `Running` | 正常译码 | 默认状态 |
| `Idle` | 无指令处理 | 队列为空 |
| `StartSquash` | 收到 squash 信号 | Commit/IEW/BAC squash |
| `Squashing` | 正在清除指令 | squash 进行中 |
| `Blocked` | 被下游阻塞 | Rename 队列满 |
| `Unblocking` | 正在解阻塞 | 下游恢复 |

---

## 二、tick() 主函数

### 2.1 执行流程

```cpp
void Decode::tick()
{
    bool status_change = false;

    for (const ThreadID tid : *activeThreads) {
        // 1. 检查来自下游阶段的信号，更新状态
        status_change = status_change || checkSignalsAndUpdate(tid);

        // 2. 执行译码
        decode(status_change, tid);
    }

    // 3. 推进时间缓冲
    toDecode->advance();

    // 4. 更新总体状态
    if (status_change) {
        updateStatus();
    }
}
```

### 2.2 checkSignalsAndUpdate() - 信号处理

Decode 接收的信号来源：

| 来源 | 信号类型 | 处理动作 |
|------|---------|---------|
| **Fetch** | 新指令到达 | 从 fetchQueue 读取指令 |
| **Rename** | 反压（队列满） | 设置 Blocked，指令移入 skidBuffer |
| **Rename** | squash | 设置 Squashing，清空队列 |
| **IEW** | squash | 设置 Squashing |
| **Commit** | squash | 设置 Squashing |

#### squash 处理

```cpp
if (fromCommit->commitInfo[tid].squash ||
    fromIEW->iewInfo[tid].squash) {
    InstSeqNum squash_seq_num = squash 的来源序列号;

    // 清空当前指令队列
    while (!insts[tid].empty()) {
        skidBuffer[tid].push(insts[tid].front());
        insts[tid].pop();
    }

    // 标记 squash 状态
    decodeStatus[tid] = Squashing;
    squashInst[tid] = ...;  // 标记最后处理的指令

    // 向 BAC 发送 squash 信号
    toDecode->decodeInfo[tid].squash = true;
    toDecode->decodeInfo[tid].nextPC = new_pc;
    wroteToTimeBuffer = true;
}
```

---

## 三、核心译码逻辑

### 3.1 decode() 函数

```cpp
void Decode::decode(bool &status_change, ThreadID tid)
{
    bool status_local_change = false;

    if (decodeStatus[tid] == Running) {
        // 正常译码模式
        decodeInsts(tid);
    } else if (decodeStatus[tid] == Unblocking) {
        // 解阻塞 - 等待滑移缓冲区清空
        if (skidsEmpty()) {
            decodeStatus[tid] = Running;
            toDecode->decodeInfo[tid].used = true;
            wroteToTimeBuffer = true;
            status_local_change = true;
        }
    } else if (decodeStatus[tid] == Squashing) {
        // Squash 完成检查
        if (squashInst[tid] == nullptr) {
            decodeStatus[tid] = Running;
            status_local_change = true;
        }
    }

    if (status_local_change) {
        status_change = true;
    }
}
```

### 3.2 decodeInsts() - 核心译码函数

```cpp
void Decode::decodeInsts(ThreadID tid)
{
    // 获取可用指令数（受 decodeWidth 限制）
    unsigned from_fetch =
        std::min(fetchQueue->fromFetch[tid].size, decodeWidth);

    // 逐条处理指令
    for (int i = 0; i < from_fetch; i++) {
        DynInstPtr inst = fetchQueue->fromFetch[tid].insts[i];

        // --- 验证 PC 相对分支预测 ---
        if (inst->isPCRelativeBranch()) {
            if (inst->predPC() != inst->pcState().nextPC()) {
                // 预测错误，触发 squash
                squash(inst, true, tid);
                return;
            }
        }

        // --- 验证控制指令类型一致性 ---
        if (inst->isControl() != inst->staticInst->isControl()) {
            // 控制类型不匹配，触发 squash
            squash(inst, false, tid);
            return;
        }

        // 添加到本地指令队列
        insts[tid].push(inst);
    }

    // --- 将指令转发到 Rename ---
    if (!insts[tid].empty()) {
        toRename->decodeInfo[tid].size = insts[tid].size();
        toRename->decodeInfo[tid].tid = tid;

        int i = 0;
        while (!insts[tid].empty()) {
            toRename->decodeInfo[tid].insts[i] = insts[tid].front();
            insts[tid].pop();
            i++;
        }

        wroteToTimeBuffer = true;
    }
}
```

### 3.3 squash() 函数

```cpp
void Decode::squash(const DynInstPtr &inst, bool branch_mispredict,
                    ThreadID tid)
{
    DPRINTF(Decode, "[tid:%i] Squashing due to branch mispredict.\n", tid);

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

---

## 四、滑移缓冲区 (Skid Buffer)

### 4.1 作用

当 Decode 被 Rename 阻塞时：
1. Fetch 仍在发送指令
2. 这些指令无法立即转发到 Rename
3. 滑移缓冲区临时存储这些指令

### 4.2 操作

```cpp
// 检查滑移缓冲区是否为空
bool skidsEmpty() const {
    return skidBuffer[tid].empty();
}

// 解阻塞时处理滑移缓冲区
void Decode::decodeInsts(ThreadID tid)
{
    // 优先处理滑移缓冲区中的指令
    if (!skidBuffer[tid].empty()) {
        while (!skidBuffer[tid].empty() && insts[tid].size() < decodeWidth) {
            insts[tid].push(skidBuffer[tid].front());
            skidBuffer[tid].pop();
        }
    }
    // 然后处理来自 Fetch 的新指令
    ...
}
```

### 4.3 大小限制

```cpp
const unsigned skidBufferMax;  // 最大滑移缓冲区大小
```

当滑移缓冲区满时，Decode 会向 Fetch 发送反压信号，阻止 Fetch 继续发送指令。

---

## 五、分支预测验证

### 5.1 PC 相对分支验证

```cpp
if (inst->isPCRelativeBranch()) {
    if (inst->predPC() != inst->pcState().nextPC()) {
        squash(inst, true, tid);
        return;
    }
}
```

- **predPC()**: Fetch 阶段预测的分支目标地址
- **pcState().nextPC()**: 实际计算的下一条 PC
- 如果不匹配，说明 Fetch 阶段的分支预测错误，需要 squash

### 5.2 控制指令类型验证

```cpp
if (inst->isControl() != inst->staticInst->isControl()) {
    squash(inst, false, tid);
    return;
}
```

- Fetch 阶段的静态指令可能将控制指令标记为非控制
- 如果实际指令类型与 Fetch 阶段不同，需要重新取指

---

## 六、阶段间通信

### 6.1 输入（来自 Fetch）

```cpp
struct FetchStruct {
    struct {
        DynInstPtr insts[MaxFetchWidth];
        int size;
        ThreadID tid;
    } fetchInfo[MaxThreads];
};
```

通过 `fetchQueue` 时间缓冲接收 Fetch 传来的指令。

### 6.2 输出（到 Rename）

```cpp
struct DecodeStruct {
    struct {
        DynInstPtr insts[MaxDecodeWidth];
        int size;
        ThreadID tid;
    } decodeInfo[MaxThreads];
};
```

通过 `decodeQueue` 时间缓冲将指令转发到 Rename。

### 6.3 反向信号（到 BAC/Fetch）

Decode 向 BAC 发送 squash 信号：
- `squash`: squash 标志
- `doneSeqNum`: squash 后的序列号
- `nextPC`: 正确的下一条 PC
- `branchMispredict`: 是否为分支预测错误
- `branchTaken`: 分支实际方向

---

## 七、Decode 在流水线中的位置

```
┌──────────┐   fetchQueue   ┌──────────┐  decodeQueue  ┌──────────┐
│  Fetch   │ ─────────────▶ │  Decode  │ ───────────▶  │  Rename  │
│          │                │          │               │          │
│ - I-cache│                │ - 验证分支               │ - 重命名  │
│ - 预译码 │                │ - 转发指令               │ - 分配物理│
│ - DynInst│                │ - squash 处理            │   寄存器  │
└──────────┘                └──────────┘               └──────────┘
```

---

## 八、关键参数

| 参数 | 说明 |
|------|------|
| `decodeWidth` | 每周期最大译码指令数 |
| `skidBufferMax` | 滑移缓冲区最大容量 |
| `fetchToDecodeDelay` | Fetch 到 Decode 的延迟周期数 |
| `renameToDecodeDelay` | Rename 反传到 Decode 的延迟 |
| `iewToDecodeDelay` | IEW 反传到 Decode 的延迟 |
| `commitToDecodeDelay` | Commit 反传到 Decode 的延迟 |

---

## 九、关键设计要点

### 9.1 Decode 为什么简单？

在 O3 CPU 中，Decode 阶段相对简单，因为：
1. **指令译码已在 Fetch 完成**: Fetch 阶段的 `decoder->decodeInst()` 已完成字节到 StaticInst 的转换
2. **主要作用是缓冲和验证**: Decode 主要作为 Fetch 和 Rename 之间的缓冲，并提供额外的分支预测验证点

### 9.2 多周期延迟

各阶段间的通信延迟参数（如 `fetchToDecodeDelay`）模拟了指令在流水线阶段之间传输的物理延迟。这些延迟通过 `TimeBuffer` 实现：

```cpp
TimeBuffer<FetchStruct> fetchQueue;   // Fetch -> Decode
TimeBuffer<DecodeStruct> decodeQueue; // Decode -> Rename
```

TimeBuffer 是一个延迟线，数据需要多个周期才能从一端到达另一端。

---

## 生成信息

- **分析范围**: Decode Stage (decode.hh/cc)
- **gem5 版本**: v25.1.0.0
