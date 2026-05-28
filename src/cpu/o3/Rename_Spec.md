# Rename Stage 规格说明

## 1. 概述

Rename（重命名）阶段是 O3 CPU 实现乱序执行的核心环节，负责：

1. 从 Decode 接收已译码的指令
2. 将架构寄存器（Architectural Register）映射为物理寄存器（Physical Register），消除 WAR 和 WAW 依赖
3. 跟踪资源空闲条目（ROB、IQ、LSQ），反压上游
4. 将重命名后的指令转发到 IEW 阶段
5. 处理 squash 恢复：通过 History Buffer 回滚寄存器映射
6. 处理序列化指令（Serialize）

## 2. 类接口

### 2.1 `class Rename`

**头文件**: `src/cpu/o3/rename.hh`

#### 构造函数
```cpp
Rename(CPU *_cpu, const BaseO3CPUParams &params);
```

#### 核心方法

| 方法 | 说明 |
|------|------|
| `void tick()` | 每周期主入口；处理 squash、解阻塞、重命名 |
| `void startupStage()` | 初始化阶段，广播空闲条目数 |
| `void clearStates(ThreadID tid)` | 清除指定线程的全部 Rename 状态 |
| `void drain()` / `void drainResume()` | 排空/恢复机制 |
| `void takeOverFrom()` | 从另一个 CPU 的线程状态接管 |
| `void deactivateThread(ThreadID tid)` | 将线程从活跃列表移除 |
| `void setTimeBuffer(TimeBuffer<TimeStruct> *tb_ptr)` | 设置反向通信时间缓冲 |
| `void setDecodeQueue(TimeBuffer<DecodeStruct> *dq_ptr)` | 设置来自 Decode 的队列 |
| `void setRenameQueue(TimeBuffer<RenameStruct> *rq_ptr)` | 设置到 IEW 的前向队列 |
| `void setFreeList(UnifiedFreeList *free_list_ptr)` | 设置物理寄存器空闲列表 |
| `void setRenameMap(UnifiedRenameMap *rename_map_ptr)` | 设置寄存器重命名映射表 |
| `void setROBCountPtr(std::vector<unsigned> *rob_count_ptr)` | 设置 ROB 空闲条目数指针 |
| `void setIQCountPtr(std::vector<unsigned> *iq_count_ptr)` | 设置 IQ 空闲条目数指针 |
| `void setLSQCountPtr(std::vector<unsigned> *lsq_count_ptr)` | 设置 LSQ 空闲条目数指针 |

## 3. 数据结构

### 3.1 状态枚举

**RenameStatus（总体状态）：**
| 状态 | 说明 |
|------|------|
| `Active` | 阶段活跃 |
| `Inactive` | 阶段不活跃 |

**ThreadStatus（每线程状态）：**
| 状态 | 说明 | 触发条件 |
|------|------|---------|
| `Running` | 正常重命名 | 默认状态 |
| `Idle` | 空闲 | 无指令可处理 |
| `Blocked` | 被阻塞 | 资源不足（ROB/IQ/LSQ 满） |
| `Unblocking` | 正在解阻塞 | 下游恢复中 |
| `Squashing` | 正在 squash | 收到 squash 信号 |

### 3.2 关键数据结构

**重命名历史记录：**
```cpp
struct RenameHistory {
    InstSeqNum instSeqNum;    // 指令序列号
    RegId archReg;            // 架构寄存器 ID
    PhysRegIdPtr newPhysReg;  // 新分配的目的物理寄存器
    PhysRegIdPtr prevPhysReg; // 之前的目的物理寄存器（用于回滚）
};
```

**空闲资源条目：**
```cpp
struct FreeEntries {
    unsigned iqEntries;       // IQ 空闲条目
    unsigned robEntries;      // ROB 空闲条目
    unsigned lqEntries;       // Load Queue 空闲条目
    unsigned sqEntries;       // Store Queue 空闲条目
};
```

**停顿源：**
```cpp
struct Stalls {
    bool iew;                 // IEW 反压
    bool serializeBlock;      // 序列化指令阻塞
};
```

**资源满标志：**
```cpp
enum FullSource {
    ROB,    // ROB 满
    IQ,     // IQ 满
    LQ,     // Load Queue 满
    SQ,     // Store Queue 满
    NONE    // 资源充足
};
```

### 3.3 关键成员变量

```cpp
RenameStatus _status;
ThreadStatus renameStatus[MaxThreads];

using InstQueue = std::deque<DynInstPtr>;
InstQueue insts[MaxThreads];                   // 重命名队列（支持 barrier）

std::list<RenameHistory> historyBuffer[MaxThreads];  // 重命名历史缓冲

// 资源跟踪
FreeEntries freeEntries[MaxThreads];
FullSource fullSource[MaxThreads];

// 停顿源
struct Stalls stalls[MaxThreads];

// Squash 相关
bool squashInst[MaxThreads];
bool changedFreeEntries[MaxThreads];
bool freeingInProgress[MaxThreads];            // squash 后物理寄存器延迟回收

// Serialize 相关
bool serializeOnNextInst[MaxThreads];           // 下一条指令需要 SerializeBefore
bool serializePending[MaxThreads];

// SMT 相关
ThreadID numThreads;
std::list<ThreadID> *activeThreads;

// 配置参数
const unsigned renameWidth;                     // 每周期最大重命名宽度
const unsigned numThreads;
const Cycles decodeToRenameDelay;               // Decode 到 Rename 的延迟
const Cycles iewToRenameDelay;                  // IEW 反传到 Rename 的延迟
const Cycles commitToRenameDelay;               // Commit 反传到 Rename 的延迟
const unsigned skidBufferMax;                   // 滑移缓冲区最大容量
const unsigned backBranchSize;                  // 反向分支大小

// 指针
UnifiedFreeList *freeList;
UnifiedRenameMap *renameMap[MaxThreads];
std::vector<unsigned> *robCountPtr;
std::vector<unsigned> *iqCountPtr;
std::vector<unsigned> *lsqCountPtr;
```

## 4. Tick 行为

### 4.1 主流程

```
tick()
  └─ 遍历每个活跃线程:
       ├─ updateStatus(tid)              // 更新线程状态
       │   └─ 检查 squash、反压、解阻塞
       │
       ├─ if squash 中:
       │    └─ doSquash(tid)             // 执行 squash 恢复
       │
       └─ if Running 或 Unblocking:
            └─ rename(status_change, tid) // 执行重命名
  └─ toRename->advance()                // 推进时间缓冲
  └─ updateOverallStatus()              // 更新总体状态
```

### 4.2 `rename()` - 重命名主逻辑

```
rename(ThreadID tid)
  ├─ 计算当前可用资源 (calcFreeEntries)
  │   ├─ 如果 IEW 反压: freeEntries = 0
  │   └─ 否则: 根据 IQ/LSQ/ROB 空闲条目计算
  │
  ├─ 如果资源不足 (freeEntries < 1):
  │    └─ 设置 Blocked，返回
  │
  ├─ 获取可重命名指令数:
  │    to_rename = min(available_instructions, renameWidth)
  │
  └─ 循环处理每条指令 (i = 0; i < to_rename):
       ├─ 如果资源已满 (freeEntries 耗尽):
       │    └─ 提前退出
       │
       ├─ 【Serialize 处理】:
       │    ├─ 如果 serializeOnNextInst 为 true:
       │    │    └─ 设置当前指令 SerializeBefore
       │    │
       │    ├─ 如果指令是 SerializeBefore:
       │    │    └─ stall = true, 序列化阻塞
       │    │
       │    └─ 如果指令是 SerializeAfter:
       │         └─ 设置 serializeOnNextInst = true
       │
       ├─ 【源寄存器重命名】renameSrcRegs():
       │    └─ 对每个源寄存器:
       │         ├─ flat_regid = 扁平化架构寄存器 ID
       │         ├─ phys_reg = renameMap[tid]->lookup(flat_regid)
       │         ├─ inst->renameSrcReg(idx, phys_reg)
       │         └─ 如果 Scoreboard 中该寄存器已就绪:
       │              └─ 标记指令源寄存器就绪
       │
       ├─ 【目的寄存器重命名】renameDestRegs():
       │    └─ 对每个目的寄存器:
       │         ├─ flat_regid = 扁平化架构寄存器 ID
       │         ├─ freeList->getReg() 获取新物理寄存器
       │         ├─ RenameInfo (new_phys, prev_phys) = renameMap[tid]->rename(flat_regid)
       │         ├─ inst->renameDestReg(idx, new_phys, prev_phys)
       │         ├─ historyBuffer.push_front({seqNum, archReg, new_phys, prev_phys})
       │         └─ Scoreboard 中设置该新物理寄存器为未就绪
       │
       ├─ 【特殊处理】handleMiscRegWaW():
       │    └─ 检测 MiscRegClass 的 WAW 依赖
       │       └─ 设置 SerializeBefore
       │
       ├─ 【资源消耗】:
       │    ├─ 根据指令类型减少 freeEntries (iqEntries/lqEntries/sqEntries)
       │    └─ 记录指令在进度中 (instsInProgress[tid]++)
       │
       └─ 推入 insts[tid] 队列
  │
  └─ 将指令转发到 IEW:
       └─ 复制 insts[tid] 到 toIEW->renameInfo[tid].insts[]
```

## 5. 寄存器重命名

### 5.1 源寄存器重命名

```cpp
void Rename::renameSrcRegs(ThreadID tid, const DynInstPtr &inst)
{
    const UnifiedRenameMap::RenameMap &map = renameMap[tid]->getMap(arch_reg.class());

    for (int idx = 0; idx < inst->numSrcRegs(); idx++) {
        const RegId& arch_reg = inst->srcRegIdx(idx);

        if (arch_reg.isRenameable()) {
            RegIndex flat_regid = renameMap[tid]->getFlatIndex(arch_reg);
            const PhysRegIdPtr& phys_reg = renameMap[tid]->lookup(arch_reg);
            inst->renameSrcReg(idx, phys_reg);

            // 检查 Scoreboard 就绪状态
            if (scoreboard->getRegState(phys_reg) == Ready) {
                inst->markSrcRegReady();
            }
        }
    }
}
```

### 5.2 目的寄存器重命名

```cpp
void Rename::renameDestRegs(ThreadID tid, const DynInstPtr &inst)
{
    for (int idx = 0; idx < inst->numDestRegs(); idx++) {
        const RegId& arch_reg = inst->destRegIdx(idx);

        if (arch_reg.isRenameable()) {
            // 从空闲列表获取新物理寄存器
            PhysRegIdPtr new_phys = freeList->getReg(arch_reg.class());

            // 重命名映射：返回 (new_phys, prev_phys)
            auto [new_phys_reg, prev_phys_reg] = renameMap[tid]->rename(arch_reg);

            inst->renameDestReg(idx, new_phys_reg, prev_phys_reg);

            // 记录到历史缓冲，用于 squash 恢复
            historyBuffer[tid].push_front({
                inst->seqNum, arch_reg, new_phys_reg, prev_phys_reg
            });

            // 在 Scoreboard 中标记新物理寄存器为未就绪
            scoreboard->setRegNotReady(new_phys_reg);
        }
    }
}
```

## 6. Squash 恢复

### 6.1 `doSquash()` - 恢复寄存器映射

```cpp
void Rename::doSquash(ThreadID tid)
{
    squashSeqNum[tid] = squash 的序列号;
    squashInst[tid] = true;

    // 遍历历史缓冲区，回滚 squash 序列号之后的重命名
    for (auto& entry : historyBuffer[tid]) {
        if (entry.instSeqNum > squashSeqNum[tid]) {
            // 回滚：将架构寄存器映射回原来的物理寄存器
            renameMap[tid]->setEntry(entry.archReg, entry.prevPhysReg);

            // 将被 squash 的物理寄存器加入回收队列
            freeList->addFreeReg(entry.newPhysReg);

            // 如果旧寄存器被 squash 后的指令使用，标记 Scoreboard 为就绪
            if (entry.prevPhysReg not needed by in-flight instructions) {
                scoreboard->setRegReady(entry.prevPhysReg);
            }
        }
    }

    // 清除 squash 历史
    squashHistory(tid);

    // squash 完成
    squashInst[tid] = false;
    renameStatus[tid] = Running;
}
```

### 6.2 `freeingInProgress` 机制

Squash 后，被回收的物理寄存器可能被仍在流水线中的指令使用。`freeingInProgress` 机制延迟回收：

```cpp
// 在 squash 时:
freeingInProgress[tid] = true;

// 在 tick() 中，当收到 Commit 的 doneSeqNum 后:
if (fromCommit->commitInfo[tid].doneSeqNum != 0) {
    removeFromHistory(tid);
    freeingInProgress[tid] = false;  // Commit 确认后可以安全回收
}
```

## 7. 序列化（Serialize）处理

### 7.1 SerializeBefore / SerializeAfter

| 类型 | 说明 | 处理方式 |
|------|------|---------|
| `SerializeBefore` | 必须等待前面的指令全部完成 | 设置阻塞标志，等待所有指令执行完 |
| `SerializeAfter` | 后面的指令必须等待本指令完成 | 设置 `serializeOnNextInst` 标志 |

### 7.2 处理流程

```cpp
// 在 rename() 中:
if (serializeOnNextInst[tid]) {
    // 上一条指令是 SerializeAfter，当前指令需要 SerializeBefore
    inst->setSerializeBefore();
    serializeOnNextInst[tid] = false;
}

if (inst->isSerializeBefore()) {
    // 序列化阻塞：等待所有在途指令完成
    stalls[tid].serializeBlock = true;
    serializePending[tid] = true;
    return;  // 阻塞重命名
}

if (inst->isSerializeAfter()) {
    // 标记下一条指令需要 SerializeBefore
    serializeOnNextInst[tid] = true;
}
```

### 7.3 MiscReg WAW 检测

```cpp
void Rename::handleMiscRegWaW(ThreadID tid, const DynInstPtr &inst)
{
    // 检测 MiscRegClass 的写后写（WAW）依赖
    // 如果发现 WAW，设置 SerializeBefore 确保顺序执行
    for (int idx = 0; idx < inst->numDestRegs(); idx++) {
        if (inst->destRegIdx(idx).is(MiscRegClass)) {
            inst->setSerializeBefore();
            serializeOnNextInst[tid] = true;
            break;
        }
    }
}
```

## 8. 资源跟踪

### 8.1 空闲条目计算

```cpp
void Rename::calcFreeEntries(ThreadID tid)
{
    if (iewBlock) {
        // IEW 反压：无法确定实际空闲条目
        freeEntries[tid] = {0, 0, 0, 0};
        fullSource[tid] = NONE;
        return;
    }

    // 计算各队列的可用条目数
    // 考虑 in-flight 的指令
    freeEntries[tid].iqEntries = iqCountPtr->at(tid) - instsInProgressIQ[tid];
    freeEntries[tid].robEntries = robCountPtr->at(tid) - instsInProgressROB[tid];
    freeEntries[tid].lqEntries = lsqCountPtr->at(tid) - instsInProgressLQ[tid];
    freeEntries[tid].sqEntries = lsqCountPtr->at(tid) - instsInProgressSQ[tid];

    // 确定哪个资源最先耗尽
    fullSource[tid] = findMinFreeEntry(freeEntries[tid]);
}
```

### 8.2 广播空闲条目

```cpp
// 在 tick() 末尾:
if (changedFreeEntries[tid]) {
    toIEW->iewInfo[tid].usedIQ = true;
    toIEW->iewInfo[tid].freeIQEntries = freeEntries[tid].iqEntries;
    toIEW->iewInfo[tid].freeLSQEntries = freeEntries[tid].sqEntries + freeEntries[tid].lqEntries;

    toRename->renameInfo[tid].freeROBEntries = freeEntries[tid].robEntries;
    toRename->renameInfo[tid].freeIQEntries = freeEntries[tid].iqEntries;
    toRename->renameInfo[tid].freeLQEntries = freeEntries[tid].lqEntries;
    toRename->renameInfo[tid].freeSQEntries = freeEntries[tid].sqEntries;
}
```

## 9. 配置参数

| 参数 | 类型 | 说明 |
|------|------|------|
| `renameWidth` | unsigned | 每周期最大重命名指令数 |
| `skidBufferMax` | unsigned | 滑移缓冲区最大容量 |
| `decodeToRenameDelay` | Cycles | Decode 到 Rename 的延迟 |
| `iewToRenameDelay` | Cycles | IEW 反传到 Rename 的延迟 |
| `commitToRenameDelay` | Cycles | Commit 反传到 Rename 的延迟 |
| `numThreads` | ThreadID | SMT 线程数 |

## 10. 统计信息

| 统计项 | 类型 | 说明 |
|--------|------|------|
| `status` | Vector | 各 Rename 状态消耗的周期数 |
| `serialized` | Scalar | 序列化指令数量 |
| `serializeStallCycles` | Scalar | 序列化阻塞的周期数 |
| `renamedInsts` | Scalar | 已重命名的指令总数 |
| `squashedInsts` | Scalar | Rename 阶段被 squash 的指令数 |
| `unusedROBEntries` | Scalar | 未使用的 ROB 条目累计 |

## 11. Probe 探针点

| 探针 | 负载类型 | 触发条件 |
|------|---------|---------|
| `Rename` | `DynInstPtr` | 指令被重命名 |
| `SquashInRename` | `DynInstPtr` | Rename 阶段被 squash |

## 12. 阶段间通信

### 12.1 输入：来自 Decode

通过 `decodeQueue` TimeBuffer 接收：

```cpp
struct DecodeStruct {
    int size;
    DynInstPtr insts[MaxWidth];
};
```

### 12.2 输出：到 IEW

通过 `renameQueue` TimeBuffer 发送：

```cpp
struct RenameStruct {
    int size;
    DynInstPtr insts[MaxWidth];
};
```

### 12.3 反向信号：来自 IEW

通过 `TimeStruct::IewComm` 接收：

| 字段 | 说明 |
|------|------|
| `freeIQEntries` | IQ 空闲条目数 |
| `freeLQEntries` | LQ 空闲条目数 |
| `freeSQEntries` | SQ 空闲条目数 |
| `usedIQ` / `usedLSQ` | IQ/LSQ 使用情况 |

### 12.4 反向信号：来自 Commit

通过 `TimeStruct::CommitComm` 接收：

| 字段 | 说明 |
|------|------|
| `squash` | Squash 信号 |
| `robSquashing` | ROB 正在 squash |
| `freeROBEntries` | ROB 空闲条目数 |
| `usedROB` | ROB 使用情况 |
| `emptyROB` | ROB 为空 |
| `doneSeqNum` | 已提交/已 squash 的序列号 |

### 12.5 阻塞/解阻塞信号

通过 `TimeStruct` 接收：
- `renameBlock[tid]`: IEW 反压阻塞
- `renameUnblock[tid]`: IEW 解阻塞

## 13. 流水线位置

```
┌──────────┐  decodeQueue   ┌──────────┐  renameQueue  ┌──────────┐
│  Decode  │ ─────────────▶ │  Rename  │ ───────────▶  │   IEW    │
│          │                │          │               │          │
│ - 验证分支               │ - 架构→物理              │ - Dispatch│
│ - 转发指令               │   寄存器重命名           │ - Execute │
│ - squash 处理            │ - 消除 WAR/WAW           │ - Writeback│
└──────────┘               │ - History Buffer         └──────────┘
                           │ - Serialize 处理
                           └──────────┘
```

## 14. 关键设计要点

1. **寄存器重命名消除假依赖** —— 通过将架构寄存器映射到不同的物理寄存器，消除了 WAR（写后读）和 WAW（写后写）假依赖，只保留真正的 RAW（读后写）依赖。

2. **History Buffer 用于快速恢复** —— squash 时，通过遍历历史缓冲区，可以快速回滚寄存器映射到 squash 点的状态，无需重新计算。

3. **`freeingInProgress` 防止提前回收** —— squash 后回收的物理寄存器可能仍在流水线中被后续指令引用。`freeingInProgress` 机制确保 Commit 确认后才真正释放这些寄存器。

4. **Serialize 指令保证顺序** —— 对于不能乱序执行的指令（如系统调用、内存屏障），通过 `SerializeBefore`/`SerializeAfter` 机制确保正确的执行顺序。

5. **资源反压** —— 当 IQ、LSQ 或 ROB 满时，Rename 停止接收新指令，通过反压信号通知上游 Decode 停止发送。

6. **InstQueue 使用 deque** —— 重命名队列使用 `std::deque` 而非 `std::queue`，以支持 barrier 指令的特殊处理需求（可以从队列中间操作）。
