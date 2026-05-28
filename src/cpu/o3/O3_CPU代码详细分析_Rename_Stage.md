# O3 CPU - Rename Stage 详细分析

## 概述

Rename（重命名）阶段是实现乱序执行的核心组件之一，负责将架构寄存器（Architectural Registers）映射到物理寄存器（Physical Registers），消除 WAR（Write-After-Read）和 WAW（Write-After-Write）数据依赖，使指令能够并行执行。

### 重命名的核心作用

1. **寄存器重命名**: 为每条指令的目的寄存器分配新的物理寄存器
2. **源寄存器解析**: 将源寄存器映射到当前最新的物理寄存器
3. **资源管理**: 跟踪 IQ、ROB、LQ、SQ 的空闲条目
4. **依赖消除**: 通过重命名消除假数据依赖

---

## 一、Rename 阶段架构

### 1.1 类定义 (rename.hh)

```cpp
class Rename
{
  public:
    enum RenameStatus {
        Active,     // 阶段活跃
        Inactive    // 阶段不活跃
    };

    enum ThreadStatus {
        Running,         // 正常重命名
        Idle,            // 空闲
        StartSquash,     // 开始 squash
        Squashing,       // 正在 squash
        Blocked,         // 被阻塞
        Unblocking,      // 正在解阻塞
        SerializeStall,  // 序列化停顿（等待 ROB 空）
        ThreadStatusMax
    };

    // 重命名历史条目
    struct RenameHistory {
        InstSeqNum instSeqNum;   // 指令序列号
        RegId archReg;           // 架构寄存器
        PhysRegIdPtr newPhysReg; // 新分配物理寄存器
        PhysRegIdPtr prevPhysReg;// 之前的物理寄存器
    };

  private:
    RenameStatus _status;
    ThreadStatus renameStatus[MaxThreads];

    // 指令队列
    typedef std::deque<DynInstPtr> InstQueue;
    InstQueue insts[MaxThreads];
    InstQueue skidBuffer[MaxThreads];

    // 重命名历史缓冲区
    std::list<RenameHistory> historyBuffer[MaxThreads];

    // 资源管理
    UnifiedRenameMap *renameMap[MaxThreads];
    UnifiedFreeList *freeList;
    Scoreboard *scoreboard;

    // 暂停源
    struct Stalls {
        bool iew;     // IEW 反压
        bool commit;  // Commit 反压
    };
    Stalls stalls[MaxThreads];

    // 资源追踪
    struct FreeEntries {
        unsigned iqEntries;   // IQ 空闲条目
        unsigned robEntries;  // ROB 空闲条目
        unsigned lqEntries;   // LQ 空闲条目
        unsigned sqEntries;   // SQ 空闲条目
    };
    FreeEntries freeEntries[MaxThreads];

    // 序列化支持
    DynInstPtr serializeInst[MaxThreads];
    bool serializeOnNextInst[MaxThreads];

    // 配置参数
    const int iewToRenameDelay;
    const int decodeToRenameDelay;
    const unsigned commitToRenameDelay;
    const unsigned renameWidth;       // 重命名宽度
    const unsigned skidBufferMax;
};
```

### 1.2 线程状态说明

| 状态 | 说明 | 触发条件 |
|------|------|---------|
| `Running` | 正常重命名 | 默认状态 |
| `Idle` | 无指令 | 队列为空 |
| `StartSquash` | 收到 squash | Commit/IEW squash |
| `Squashing` | 正在清除 | squash 进行中 |
| `Blocked` | 被下游阻塞 | IEW 队列满 |
| `Unblocking` | 正在解阻塞 | 下游恢复 |
| `SerializeStall` | 等待序列化指令提交 | 序列化指令在 ROB 中 |

---

## 二、核心数据结构

### 2.1 物理寄存器文件 (PhysRegFile)

```cpp
class PhysRegFile
{
    // 各类物理寄存器
    std::vector<std::vector<uint8_t>> regs;

    RegVal getReg(PhysRegIdPtr phys_reg);
    void setReg(PhysRegIdPtr phys_reg, RegVal val);
    void *getWritableReg(PhysRegIdPtr phys_reg);
};
```

### 2.2 统一空闲列表 (UnifiedFreeList)

```cpp
class UnifiedFreeList
{
    class SimpleFreeList {
        std::list<PhysRegIdPtr> freeList;
        unsigned numFreeRegs;
        PhysRegIdPtr getReg();
        void addReg(PhysRegIdPtr free_reg);
    };

    SimpleFreeList *freeLists[Num_RegClassType];

    PhysRegIdPtr getReg(RegClassType type);
    void addReg(RegClassType type, PhysRegIdPtr free_reg);
    unsigned numFreeRegs(RegClassType type);
};
```

- **IntRegClass**: 整数寄存器空闲列表
- **FloatRegClass**: 浮点寄存器空闲列表
- **VecRegClass**: 向量寄存器空闲列表
- **VecPredRegClass**: 向量谓词寄存器
- **MatRegClass**: 矩阵寄存器
- **CCRegClass**: 条件码寄存器

### 2.3 统一重命名映射 (UnifiedRenameMap)

```cpp
class UnifiedRenameMap
{
    class SimpleRenameMap {
        std::vector<PhysRegIdPtr> map;  // arch_reg → phys_reg
        PhysRegIdPtr lookup(const RegId &arch_reg);
        void setEntry(const RegId &arch_reg, PhysRegIdPtr phys_reg);
    };

    SimpleRenameMap *maps[Num_RegClassType];

    PhysRegIdPtr lookup(const RegId &arch_reg);
    void setEntry(const RegId &arch_reg, PhysRegIdPtr phys_reg);
};
```

每个线程维护独立的重命名映射表。

### 2.4 记分板 (Scoreboard)

```cpp
class Scoreboard
{
    std::vector<uint8_t> regScoreboard;  // 位图

    bool getReg(PhysRegIdPtr phys_reg);   // 检查就绪
    void setReg(PhysRegIdPtr phys_reg);   // 标记就绪
    void unsetReg(PhysRegIdPtr phys_reg); // 标记未就绪
};
```

记分板跟踪每个物理寄存器是否已写入最新值：
- **setReg**: 寄存器值已写入（就绪）
- **unsetReg**: 寄存器正在被写，值未就绪
- **getReg**: 查询寄存器是否就绪

---

## 三、tick() 主函数

### 3.1 执行流程

```cpp
void Rename::tick()
{
    bool status_change = false;

    for (const ThreadID tid : *activeThreads) {
        // 1. 检查信号并更新状态
        status_change = status_change || checkSignalsAndUpdate(tid);

        // 2. 执行重命名
        rename(status_change, tid);
    }

    // 3. 推进时间缓冲
    toRename->advance();

    // 4. 更新状态
    if (status_change) {
        updateStatus();
    }
}
```

### 3.2 checkSignalsAndUpdate() - 信号处理

```cpp
bool Rename::checkSignalsAndUpdate(ThreadID tid)
{
    bool status_change = false;

    // --- 来自 Commit 的信号 ---
    if (fromCommit->commitInfo[tid].squash) {
        squash(fromCommit->commitInfo[tid].doneSeqNum, tid);
        squashIt(tid);  // squash ROB 中的指令
        status_change = true;
    }

    // --- 来自 IEW 的信号 ---
    if (fromIEW->iewInfo[tid].squash) {
        squash(fromIEW->iewInfo[tid].doneSeqNum, tid);
        status_change = true;
    }

    // --- 来自 IEW 的反压 ---
    if (fromIEW->iewInfo[tid].used && stalls[tid].iew) {
        stalls[tid].iew = false;
        if (!stalls[tid].commit) {
            unblock(tid);
            status_change = true;
        }
    }

    // --- 来自 Commit 的反压 ---
    if (fromCommit->commitInfo[tid].used && stalls[tid].commit) {
        stalls[tid].commit = false;
        if (!stalls[tid].iew) {
            unblock(tid);
            status_change = true;
        }
    }

    // --- 更新空闲资源计数 ---
    if (fromIEW->iewInfo[tid].used) {
        freeEntries[tid].iqEntries = fromIEW->iewInfo[tid].iqCount;
        freeEntries[tid].robEntries = fromIEW->iewInfo[tid].robCount;
        freeEntries[tid].lqEntries = fromIEW->iewInfo[tid].lqCount;
        freeEntries[tid].sqEntries = fromIEW->iewInfo[tid].sqCount;
    }

    return status_change;
}
```

---

## 四、核心重命名逻辑

### 4.1 rename() 函数

```cpp
void Rename::rename(bool &status_change, ThreadID tid)
{
    bool status_local_change = false;

    if (renameStatus[tid] == Running) {
        renameInsts(tid);
    } else if (renameStatus[tid] == Unblocking) {
        if (skidBuffer[tid].empty()) {
            renameStatus[tid] = Running;
            toRename->renameInfo[tid].used = true;
            wroteToTimeBuffer = true;
            status_local_change = true;
        }
    } else if (renameStatus[tid] == SerializeStall) {
        // 序列化停顿 - 等待 ROB 为空
        if (rob->isEmpty(tid)) {
            renameStatus[tid] = Unblocking;
            status_local_change = true;
        }
    } else if (renameStatus[tid] == Squashing) {
        if (squashInst[tid] == nullptr) {
            renameStatus[tid] = Running;
            status_local_change = true;
        }
    }

    if (status_local_change) {
        status_change = true;
    }
}
```

### 4.2 renameInsts() - 核心重命名函数

```cpp
void Rename::renameInsts(ThreadID tid)
{
    // 获取可重命名指令数（受 renameWidth 和 IQ 空闲条目限制）
    unsigned num_insts = std::min(
        static_cast<unsigned>(insts[tid].size()),
        std::min(renameWidth,
            static_cast<unsigned>(freeEntries[tid].iqEntries)));

    for (int i = 0; i < num_insts; i++) {
        DynInstPtr inst = insts[tid].front();
        insts[tid].pop_front();

        // --- 资源检查 ---
        if (freeEntries[tid].robEntries == 0 ||
            freeEntries[tid].iqEntries == 0 ||
            freeEntries[tid].lqEntries == 0 ||
            freeEntries[tid].sqEntries == 0) {
            // 资源不足，放回队列
            insts[tid].push_front(inst);
            block(tid);
            break;
        }

        // --- 序列化检查 ---
        if (inst->isSerializeBefore()) {
            serializeAfter(insts[tid], tid);
        }

        if (serializeInst[tid] != nullptr) {
            if (inst->seqNum == serializeInst[tid]->seqNum + 1) {
                serializeInst[tid] = nullptr;
                renameStatus[tid] = Unblocking;
            } else {
                insts[tid].push_front(inst);
                break;
            }
        }

        // --- 目的寄存器重命名 ---
        if (inst->isFirstMicroop() || inst->isLastMicroop() ||
            (!inst->isMicroop() && inst->hasDestReg())) {
            renameDestRegs(inst, tid);
        }

        // --- 源寄存器重命名 ---
        if (!inst->isNoop() && inst->hasSrcReg()) {
            renameSrcRegs(inst, tid);
        }

        // --- 更新资源计数 ---
        if (inst->isLoad()) {
            freeEntries[tid].lqEntries--;
        } else if (inst->isStore()) {
            freeEntries[tid].sqEntries--;
        } else if (!inst->isNoop() && !inst->isMemBarrier()) {
            freeEntries[tid].iqEntries--;
        }
        freeEntries[tid].robEntries--;

        // --- 标记指令可提交 ---
        inst->setCanCommit();

        // --- 发送到 IEW ---
        toIEW->renameInfo[tid].insts[toIEWIndex] = inst;
        toIEW->renameInfo[tid].size++;
        wroteToTimeBuffer = true;

        toIEWIndex++;
        if (toIEWIndex >= renameWidth) {
            toIEWIndex = 0;
        }

        instsInProgress[tid]++;
        if (inst->isLoad()) loadsInProgress[tid]++;
        if (inst->isStore()) storesInProgress[tid]++;
    }
}
```

### 4.3 renameDestRegs() - 目的寄存器重命名

```cpp
void Rename::renameDestRegs(const DynInstPtr &inst, ThreadID tid)
{
    int num_dest_regs = inst->numDestRegs();

    for (int i = 0; i < num_dest_regs; i++) {
        RegId arch_reg = inst->destRegIdx(i);
        PhysRegIdPtr phys_reg;
        PhysRegIdPtr prev_phys_reg;

        // 1. 从空闲列表分配新的物理寄存器
        phys_reg = freeList->getReg(arch_reg.classType());

        // 2. 查找当前架构寄存器映射
        prev_phys_reg = renameMap[tid].lookup(arch_reg);

        // 3. 更新重命名映射
        renameMap[tid].setEntry(arch_reg, phys_reg);

        // 4. 标记新寄存器为未就绪（等待写回）
        scoreboard->unsetReg(phys_reg);

        // 5. 记录重命名历史（用于 squash 恢复）
        RenameHistory history_entry(
            inst->seqNum, arch_reg, phys_reg, prev_phys_reg);
        historyBuffer[tid].push_back(history_entry);

        // 6. 设置指令的目的寄存器
        inst->setDestReg(i, phys_reg);
    }

    stats.renamedOperands += num_dest_regs;
}
```

### 4.4 renameSrcRegs() - 源寄存器重命名

```cpp
void Rename::renameSrcRegs(const DynInstPtr &inst, ThreadID tid)
{
    int num_src_regs = inst->numSrcRegs();

    for (int i = 0; i < num_src_regs; i++) {
        RegId arch_reg = inst->srcRegIdx(i);
        PhysRegIdPtr phys_reg;

        // 查找当前架构寄存器对应的物理寄存器
        phys_reg = renameMap[tid].lookup(arch_reg);

        // 设置指令的源寄存器
        inst->setSrcReg(i, phys_reg);
    }

    stats.lookups += num_src_regs;
}
```

---

## 五、Squash 处理

### 5.1 squash() 函数

```cpp
void Rename::squash(const InstSeqNum &squash_seq_num, ThreadID tid)
{
    DPRINTF(Rename, "[tid:%i] Squashing until seq_num %i.\n",
            tid, squash_seq_num);

    // 将未处理的指令移到滑移缓冲区
    while (!insts[tid].empty()) {
        skidBuffer[tid].push_back(insts[tid].front());
        insts[tid].pop_front();
    }

    renameStatus[tid] = Squashing;
    wroteToTimeBuffer = true;
}
```

### 5.2 doSquash() - 恢复重命名历史

```cpp
void Rename::doSquash(const InstSeqNum &squash_seq_num, ThreadID tid)
{
    bool done = true;

    // 从重命名历史缓冲区恢复
    while (!historyBuffer[tid].empty()) {
        RenameHistory &history_entry = historyBuffer[tid].back();

        if (history_entry.instSeqNum > squash_seq_num) {
            // 1. 将新分配物理寄存器返回空闲列表
            freeList->addReg(history_entry.archReg.classType(),
                          history_entry.newPhysReg);

            // 2. 恢复旧的重命名映射
            renameMap[tid].setEntry(history_entry.archReg,
                                    history_entry.prevPhysReg);

            historyBuffer[tid].pop_back();
        } else {
            done = false;
            break;
        }
    }

    if (done) {
        squashInst[tid] = nullptr;
    }
}
```

### 5.3 Squash 恢复流程

```
historyBuffer (后进先出):
  ┌────────────────────────────────────────────┐
  │ seq=100, x1→p5, old=p3  ← 最后重命名       │
  │ seq=99,  x2→p6, old=p4                     │
  │ seq=98,  x1→p3, old=p1                     │
  │ seq=97,  x3→p7, old=p2  ← 最早重命名       │
  └────────────────────────────────────────────┘

假设 squash_seq_num = 98:
  - seq=100 > 98 → 恢复: freeList.add(p5), renameMap[x1]=p3
  - seq=99 > 98  → 恢复: freeList.add(p6), renameMap[x2]=p4
  - seq=98 ≤ 98  → 停止
```

---

## 六、序列化停顿 (SerializeStall)

某些指令（如系统调用、特权指令、内存屏障）需要序列化执行，即在它们之前的所有指令都完成后才能执行。

### 6.1 serializeAfter()

```cpp
void Rename::serializeAfter(const InstQueue &insts, ThreadID tid)
{
    // 标记下一条指令为序列化后的指令
    serializeOnNextInst[tid] = true;
}
```

当遇到 `isSerializeBefore()` 指令时：
1. 暂停后续指令的重命名
2. 设置 `SerializeStall` 状态
3. 等待 ROB 为空（所有之前指令已提交）
4. 恢复重命名

---

## 七、阶段间通信

### 7.1 输入（来自 Decode）

```cpp
struct DecodeStruct {
    struct {
        DynInstPtr insts[MaxDecodeWidth];
        int size;
        ThreadID tid;
    } decodeInfo[MaxThreads];
};
```

通过 `decodeQueue` 时间缓冲接收指令。

### 7.2 输出（到 IEW）

```cpp
struct RenameStruct {
    struct {
        DynInstPtr insts[MaxRenameWidth];
        int size;
        ThreadID tid;
    } renameInfo[MaxThreads];
};
```

通过 `renameQueue` 时间缓冲发送已重命名指令。

### 7.3 接收反向信号

| 来源 | 信号 | 动作 |
|------|------|------|
| IEW | squash | squash 并恢复重命名映射 |
| Commit | squash | squash 并恢复重命名映射 |
| IEW | 反压 | 阻塞，指令进入 skidBuffer |
| Commit | 反压 | 阻塞，指令进入 skidBuffer |
| IEW | 资源更新 | 更新 freeEntries 计数 |

---

## 八、重命名流程图

```
┌──────────────────────────────────────────────────────────────┐
│                      Rename Stage                             │
│                                                               │
│  1. checkSignalsAndUpdate()                                   │
│     ├── squash? → squash(), 恢复 historyBuffer                │
│     ├── blocked? → block(), 指令入 skidBuffer                 │
│     └── unblocked? → unblock()                                │
│                                                               │
│  2. renameInsts() (每周期最多 renameWidth 条)                  │
│     ├── 资源检查: IQ/ROB/LQ/SQ 是否有空位                     │
│     ├── 序列化检查: 是否有 serialize 指令                     │
│     ├── renameDestRegs():                                     │
│     │   ├── freeList.getReg() 分配新物理寄存器                │
│     │   ├── renameMap.setEntry() 更新映射                     │
│     │   ├── scoreboard.unsetReg() 标记未就绪                  │
│     │   └── historyBuffer.push() 记录历史                     │
│     ├── renameSrcRegs():                                      │
│     │   └── renameMap.lookup() 查找物理寄存器                │
│     ├── 更新资源计数                                          │
│     └── inst.setCanCommit()                                   │
│                                                               │
│  3. 写入 timeBuffer → toIEW                                   │
│                                                               │
└──────────────────────────────────────────────────────────────┘
```

---

## 九、关键参数

| 参数 | 说明 |
|------|------|
| `renameWidth` | 每周期最大重命名指令数 |
| `skidBufferMax` | 滑移缓冲区最大容量 |
| `numPhysIntRegs` | 物理整数寄存器数 |
| `numPhysFloatRegs` | 物理浮点寄存器数 |
| `numPhysVecRegs` | 物理向量寄存器数 |
| `numPhysVecPredRegs` | 物理向量谓词寄存器数 |
| `numPhysMatRegs` | 物理矩阵寄存器数 |
| `numPhysCCRegs` | 物理条件码寄存器数 |
| `decodeToRenameDelay` | Decode 到 Rename 延迟 |
| `iewToRenameDelay` | IEW 反传到 Rename 延迟 |
| `commitToRenameDelay` | Commit 反传到 Rename 延迟 |

---

## 十、重命名示例

### 10.1 消除 WAW 和 WAR 依赖

```assembly
# 原始代码 (架构寄存器)
I1: ADD  x1, x2, x3    # 写 x1
I2: ADD  x1, x4, x5    # 写 x1 (WAW 依赖 I1)
I3: ADD  x6, x1, x7    # 读 x1 (RAW 依赖 I2, 非 I1)
I4: ADD  x8, x1, x9    # 读 x1 (WAR 依赖 I2 后的 x1)

# 重命名后 (物理寄存器)
I1: ADD  p10, p2, p3    # 写 p10, renameMap[x1]=p10
I2: ADD  p11, p4, p5    # 写 p11, renameMap[x1]=p11 (p10 不再代表 x1)
I3: ADD  p6, p11, p7    # 读 p11 (来自 I2 的结果)
I4: ADD  p8, p11, p9    # 读 p11 (来自 I2 的结果)

# 结果:
# - I2 不再 WAW 依赖 I1 (不同物理寄存器)
# - I4 不再 WAR 依赖 I2 (I4 读 I2 的 p11，但 I2 不影响 I4 的 p8)
```

---

## 生成信息

- **分析范围**: Rename Stage (rename.hh/cc)
- **gem5 版本**: v25.1.0.0
