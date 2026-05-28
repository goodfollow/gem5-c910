# Phase 1 修改总结：多 Issue Queue 架构改造

## 修改文件清单

| 文件 | 修改内容 |
|------|---------|
| `inst_queue.hh` | 新增 IQType 枚举、IQ 类型路由/检查接口、AIQ 轮转计数器 |
| `inst_queue.cc` | 实现 IQType 解析、findIQByType/isFullByType/insertToIQType |
| `IQUnit.py` | 新增 iqType 参数和 IQType 常量类 |
| `BaseO3CPU.py` | 新增 8 个 per-IQ-type 容量参数 |
| `iew.cc` | 重写 dispatchInsts() 路由逻辑 |

---

## 1. inst_queue.hh — IQType 枚举 + IQUnit 类型标记 + 路由接口

### 1.1 IQType 枚举定义

```cpp
enum class IQType {
    AIQ0,           // Integer ALU queue 0 (11 entries)
    AIQ1,           // Integer ALU queue 1 (11 entries, has MLA unit)
    BIQ,            // Branch/Jump queue (12 entries)
    LSIQ,           // Load/Store address queue (16 entries)
    SDIQ,           // Store data queue (8 entries)
    VIQ0,           // Vector ALU queue 0 (10 entries)
    VIQ1,           // Vector ALU queue 1 (10 entries, has VFMAU)
    VMB,            // Vector memory buffer queue (8 entries)
    NUM_IQ_TYPES
};

inline const char*
to_string(IQType type)
{
    static const char* names[] = {
        "AIQ0", "AIQ1", "BIQ", "LSIQ", "SDIQ", "VIQ0", "VIQ1", "VMB"
    };
    return names[static_cast<int>(type)];
}
```

### 1.2 IQUnit 新增成员

```cpp
// 新增 IQType getter
IQType iqType() const { return _iqType; }

// 新增 private 成员
IQType _iqType;  // The type of this IQ (AIQ0, AIQ1, BIQ, etc.)
```

### 1.3 InstructionQueue 新增公共接口

```cpp
// 按 IQ 类型查找有空闲 entry 的 IQ
IQUnit *findIQByType(IQType type, ThreadID tid);

// 检查指定类型的 IQ 是否已满
bool isFullByType(IQType type, ThreadID tid);

// 将指令插入指定类型的 IQ
bool insertToIQType(const DynInstPtr &new_inst, IQType type);

// AIQ 轮转计数器 accessor
int getAIQRRCounter() const { return aiqRRCounter; }
void advanceAIQRRCounter() { aiqRRCounter = (aiqRRCounter + 1) % 2; }
```

### 1.4 InstructionQueue 新增 private 成员

```cpp
int aiqRRCounter;  // Round-robin selector for AIQ0/AIQ1 load balancing
```

---

## 2. inst_queue.cc — 实现 IQType 解析 + 路由方法

### 2.1 IQType 字符串解析函数

```cpp
static IQType
parseIQType(const std::string &s)
{
    static const std::map<std::string, IQType> table = {
        {"AIQ0", IQType::AIQ0},
        {"AIQ1", IQType::AIQ1},
        {"BIQ",  IQType::BIQ},
        {"LSIQ", IQType::LSIQ},
        {"SDIQ", IQType::SDIQ},
        {"VIQ0", IQType::VIQ0},
        {"VIQ1", IQType::VIQ1},
        {"VMB",  IQType::VMB},
    };
    auto it = table.find(s);
    if (it == table.end())
        panic("Unknown IQType '%s'", s);
    return it->second;
}
```

### 2.2 IQUnit 构造函数初始化 `_iqType`

```cpp
IQUnit::IQUnit(const IQUnitParams &params)
    : SimObject(params),
      iqPolicy(params.smtIQPolicy),
      _iqType(parseIQType(params.iqType)),  // 新增
      numThreads(params.numThreads),
      ...
```

### 2.3 InstructionQueue 构造函数初始化 aiqRRCounter

```cpp
InstructionQueue::InstructionQueue(CPU *cpu_ptr, IEW *iew_ptr,
                                   const BaseO3CPUParams &params)
    : cpu(cpu_ptr),
      ...
      commitToIEWDelay(params.commitToIEWDelay),
      aiqRRCounter(0),  // 新增
      iqStats(cpu, totalWidth),
      ...
```

### 2.4 findIQByType / isFullByType / insertToIQType 实现

```cpp
IQUnit *
InstructionQueue::findIQByType(IQType type, ThreadID tid)
{
    for (auto iq : iqs) {
        if (iq->iqType() == type && iq->numFreeEntries(tid) > 0) {
            return iq;
        }
    }
    return nullptr;
}

bool
InstructionQueue::isFullByType(IQType type, ThreadID tid)
{
    for (auto iq : iqs) {
        if (iq->iqType() == type && iq->numFreeEntries(tid) > 0) {
            return false;
        }
    }
    return true;
}

bool
InstructionQueue::insertToIQType(const DynInstPtr &new_inst, IQType type)
{
    auto iq = findIQByType(type, new_inst->threadNumber);
    if (!iq)
        return false;

    // IO 统计
    if (new_inst->isFloating()) {
        iqIOStats.fpInstQueueWrites++;
    } else if (new_inst->isVector()) {
        iqIOStats.vecInstQueueWrites++;
    } else {
        iqIOStats.intInstQueueWrites++;
    }

    assert(new_inst);
    DPRINTF(IQ, "Adding instruction [sn:%llu] PC %s to IQ type %s.\n",
            new_inst->seqNum, new_inst->pcState(), to_string(type));

    instList[new_inst->threadNumber].push_back(new_inst);
    iq->insert(new_inst);

    addToDependents(new_inst);
    addToProducers(new_inst);

    if (new_inst->isMemRef()) {
        memDepUnit[new_inst->threadNumber].insert(new_inst);
    } else {
        addIfReady(new_inst);
    }

    ++iqStats.instsAdded;
    return true;
}
```

---

## 3. IQUnit.py — 新增 iqType 参数

```python
class IQType:
    """Issue Queue types aligned with OpenC910 architecture."""
    AIQ0 = "AIQ0"
    AIQ1 = "AIQ1"
    BIQ = "BIQ"
    LSIQ = "LSIQ"
    SDIQ = "SDIQ"
    VIQ0 = "VIQ0"
    VIQ1 = "VIQ1"
    VMB = "VMB"


class IQUnit(SimObject):
    type = "IQUnit"
    cxx_class = "gem5::o3::IQUnit"
    cxx_header = "cpu/o3/inst_queue.hh"

    numEntries = Param.Unsigned(64, "Number of instruction queue entries")

    iqType = Param.String("AIQ0", "IQ type: AIQ0, AIQ1, BIQ, LSIQ, SDIQ, VIQ0, VIQ1, VMB")

    fuPool = Param.FUPool(DefaultFUPool(), "Functional Unit pool")

    numThreads = Param.Unsigned(
        Parent.numThreads, "number of HW thread contexts"
    )

    smtIQPolicy = Param.SMTQueuePolicy("Partitioned", "SMT IQ Sharing Policy")
    smtIQThreshold = Param.Int(100, "SMT IQ Threshold Sharing Parameter")
```

---

## 4. BaseO3CPU.py — 新增 per-IQ-type 容量参数

```python
# 在 numROBEntries 之后新增：
    # Per-IQ-type capacity parameters (OpenC910 alignment)
    aiq0Entries = Param.Unsigned(11, "AIQ0 entries (integer ALU)")
    aiq1Entries = Param.Unsigned(11, "AIQ1 entries (integer ALU + MLA)")
    biqEntries  = Param.Unsigned(12, "BIQ entries (branch/jump)")
    lsiqEntries = Param.Unsigned(16, "LSIQ entries (load/store address)")
    sdiqEntries = Param.Unsigned(8,  "SDIQ entries (store data)")
    viq0Entries = Param.Unsigned(10, "VIQ0 entries (vector ALU)")
    viq1Entries = Param.Unsigned(10, "VIQ1 entries (vector ALU + VFMAU)")
    vmbEntries  = Param.Unsigned(8,  "VMB entries (vector memory buffer)")
```

---

## 5. iew.cc — dispatchInsts() 路由逻辑重写

### 5.1 新增 include

```cpp
#include "cpu/o3/inst_queue.hh"
```

### 5.2 路由变量声明

```cpp
IQType targetIQ = IQType::AIQ0;
bool add_to_iq = false;
```

### 5.3 指令类型 → IQ 路由规则

```cpp
// 在事务内存状态处理之后，重写指令分发逻辑：

add_to_iq = false;

if (inst->isAtomic()) {
    // Atomic → LSQ + insertNonSpec (不变)
    ldstQueue.insertStore(inst);
    inst->setCanCommit();
    instQueue.insertNonSpec(inst);

} else if (inst->isLoad()) {
    // Load → LSIQ (地址计算)
    ldstQueue.insertLoad(inst);
    targetIQ = IQType::LSIQ;
    add_to_iq = true;

} else if (inst->isStore()) {
    if (inst->isStoreConditional()) {
        // Store Conditional → insertNonSpec (不变)
        inst->setCanCommit();
        instQueue.insertNonSpec(inst);
    } else {
        // 普通 Store 数据 → SDIQ
        ldstQueue.insertStore(inst);
        targetIQ = IQType::SDIQ;
        add_to_iq = true;
    }

} else if (inst->isReadBarrier() || inst->isWriteBarrier()) {
    // Barrier → insertBarrier (不变)
    inst->setCanCommit();
    instQueue.insertBarrier(inst);

} else if (inst->isNop()) {
    // Nop → 直接标记完成 (不变)
    inst->setIssued();
    inst->setExecuted();
    inst->setCanCommit();
    instQueue.recordProducer(inst);

} else {
    // 非内存指令：按类型路由到专用 IQ
    add_to_iq = true;
    enums::OpClass opClass = inst->opClass();
    bool isBranch = inst->isControl();

    if (isBranch) {
        // 分支/跳转 → BIQ
        targetIQ = IQType::BIQ;

    } else if (inst->isVector()) {
        // 向量指令
        if (opClass == enums::VectorMisc || opClass == enums::VectorMem) {
            // 向量内存 → VMB
            targetIQ = IQType::VMB;
        } else if (opClass == enums::VectorFloatDiv ||
                   opClass == enums::VectorFloatSqrt ||
                   opClass == enums::VectorFloatMult) {
            // 向量乘法 → VIQ1 (VFMAU)
            targetIQ = IQType::VIQ1;
        } else {
            // 向量 ALU → VIQ0
            targetIQ = IQType::VIQ0;
        }

    } else if (inst->isFloating()) {
        // 标量浮点 → VIQ1
        targetIQ = IQType::VIQ1;

    } else {
        // 整数 ALU 指令
        if (opClass == enums::IntMult ||
            opClass == enums::FloatMult ||
            opClass == enums::FloatMultAcc) {
            // MUL/MADD → AIQ1 (有 MLA 单元)
            targetIQ = IQType::AIQ1;
        } else if (opClass == enums::IntDiv ||
                   opClass == enums::FloatDiv ||
                   opClass == enums::FloatSqrt) {
            // DIV/REM → AIQ1
            targetIQ = IQType::AIQ1;
        } else {
            // 其他整数 → AIQ0/AIQ1 轮转
            targetIQ = (instQueue.getAIQRRCounter() % 2 == 0)
                ? IQType::AIQ0 : IQType::AIQ1;
            instQueue.advanceAIQRRCounter();
        }
    }
}
```

### 5.4 插入逻辑更新

```cpp
// 将原来的 instQueue.insert(inst) 替换为：
if (add_to_iq) {
    if (!instQueue.insertToIQType(inst, targetIQ)) {
        DPRINTF(IEW, "[tid:%i] Issue: IQ type %s is full.\n",
                tid, to_string(targetIQ));
        block(tid);
        toRename->iewUnblock[tid] = false;
        ++iewStats.iqFullEvents;
        break;
    }
}
```

---

## 路由规则汇总表

| 指令类型 | 目标 IQ | 条件 |
|---------|---------|------|
| Atomic | NonSpec (LSQ) | `isAtomic()` |
| Load | LSIQ | `isLoad()` |
| Store Conditional | NonSpec (LSQ) | `isStoreConditional()` |
| 普通 Store | SDIQ | `isStore()` 且非 SC |
| Read/Write Barrier | NonSpec (Barrier) | `isReadBarrier()`/`isWriteBarrier()` |
| Nop | 直接完成 | `isNop()` |
| 分支/跳转 | BIQ | `isControl()` |
| 向量内存 | VMB | `isVector()` + VectorMisc/VectorMem |
| 向量乘加 | VIQ1 | `isVector()` + VectorFloatDiv/Sqrt/Mult |
| 向量 ALU | VIQ0 | `isVector()` 其他 |
| 标量浮点 | VIQ1 | `isFloating()` |
| 整数乘加 | AIQ1 | IntMult/FloatMult/FloatMultAcc |
| 整数除法 | AIQ1 | IntDiv/FloatDiv/FloatSqrt |
| 整数 ALU | AIQ0/AIQ1 轮转 | 其他整数指令 |
