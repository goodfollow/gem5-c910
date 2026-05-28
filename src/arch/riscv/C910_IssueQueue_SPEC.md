# C910 Issue Queue 规格移植到 gem5 O3 CPU

**文档版本**: 1.0
**适用架构**: RISC-V (gem5 O3 CPU)
**参考模型**: OpenC910 IssueQueue (REV=1, SUB_VER=4, PATCH=3)
**创建日期**: 2026-04-08

---

## 修订历史

| 版本 | 日期       | 作者      | 修改描述                           |
|:----:|:----------:|:---------:|:-----------------------------------|
| 1.0  | 2026-04-08 | gem5 Team | 初始版本，基于 C910 IQ SPEC 移植到 gem5 |

---

## 目录

1. [概述](#1-概述)
2. [架构规格](#2-架构规格)
3. [数据结构](#3-数据结构)
4. [接口定义](#4-接口定义)
5. [操作流程](#5-操作流程)
6. [状态机定义](#6-状态机定义)
7. [时序规范](#7-时序规范)
8. [配置参数](#8-配置参数)
9. [实现建议](#9-实现建议)

---

## 1. 概述

### 1.1 功能描述

Issue Queue (IS) 是指令分发单元 (IDU) 中的核心组件，负责缓存已译码的指令，并在操作数就绪后发射到相应的执行单元。C910 采用多发射、乱序执行架构，Issue Queue 的设计直接决定了指令级并行度 (ILP) 的挖掘能力。

### 1.2 设计特点

| 特性 | 描述 |
|:-----|:-----|
| 多 Queue 设计 | 根据指令类型分发到不同的 Issue Queue |
| 集中式仲裁 | 每个 Queue 独立仲裁，选择就绪指令发射 |
| 操作数前递 | 支持从执行单元直接前递数据到 Issue Queue |
| 动态就绪检查 | 实时监测源操作数就绪状态 |
| 年龄向量管理 | 维护指令相对年龄，确保公平发射 |

### 1.3 在流水线中的位置

```
┌─────────────┬─────────────┬─────────────┬─────────────┬─────────────┐
│    Fetch    │    Decode   │    Rename   │     IS      │   Commit    │
│             │             │             │  (IDU Stage)│             │
│             │             │             │  ┌───────┐  │             │
│             │             │             │  │ Issue │  │             │
│             │             │             │  ├───────┤  │             │
│             │             │             │  │Execute│  │             │
│             │             │             │  ├───────┤  │             │
│             │             │             │  │Writebk│  │             │
│             │             │             │  └───────┘  │             │
│             │             │             └─────────────┘             │
└─────────────┴─────────────┴─────────────────────────────────────────┘
                              ↑
                              │
                        Dispatch
```

---

## 2. 架构规格

### 2.1 Issue Queue 类型与配置

基于 C910 的多 Queue 设计，gem5 O3 CPU 应支持以下 Issue Queue:

| Queue 名称 | 全称                  | C910 入口数 | C910 发射宽度 | 功能描述               |
|:-----------|:----------------------|:-----------:|:-------------:|:-----------------------|
| AIQ0       | ALU Issue Queue 0     | 11          | 2             | 整数 ALU 指令 (Pipe0)    |
| AIQ1       | ALU Issue Queue 1     | 11          | 2             | 整数 ALU 指令 (Pipe1)    |
| BIQ        | Branch Issue Queue    | 12          | 2             | 分支跳转指令 (Pipe2)     |
| LSIQ       | Load/Store Issue Queue| 16          | 2             | 加载指令 (Pipe3/4)       |
| SDIQ       | Store Data Issue Queue| 8           | 2             | 存储数据指令 (Pipe5)     |
| VIQ0       | Vector Issue Queue 0  | 10          | 2             | 向量指令 (Pipe6)         |
| VIQ1       | Vector Issue Queue 1  | 10          | 2             | 向量指令 (Pipe7)         |
| VMB        | Vector Memory Buffer  | 8           | 2             | 向量存储缓冲             |

### 2.2 gem5 映射建议

在 gem5 O3 CPU 中，可以通过以下方式实现类似的多 Queue 架构:

```python
# RISC-V O3 CPU 多 Queue 配置
class RiscvO3CPU(BaseO3CPU, RiscvCPU):
    # 多 IQUnit 配置 - 模拟 C910 的多 Queue 设计
    instQueues = VectorParam.IQUnit(
        [
            IQUnit(numEntries=11, fuPool=ALUFUPool(), name="AIQ0"),
            IQUnit(numEntries=11, fuPool=ALUFUPool(), name="AIQ1"),
            IQUnit(numEntries=12, fuPool=BranchFUPool(), name="BIQ"),
            IQUnit(numEntries=16, fuPool=LoadFUPool(), name="LSIQ"),
            IQUnit(numEntries=8, fuPool=StoreFUPool(), name="SDIQ"),
            IQUnit(numEntries=10, fuPool=VectorFUPool(), name="VIQ0"),
            IQUnit(numEntries=10, fuPool=VectorFUPool(), name="VIQ1"),
        ],
        size=7
    )

    # 发射宽度配置 (总发射宽度=8)
    issueWidth = 8
    dispatchWidth = 8
```

### 2.3 物理寄存器文件 (PRF) 接口

| 寄存器文件 | 位宽   | 入口数 | 功能           |
|:-----------|:-------|:------:|:---------------|
| EREG       | 64-bit | 64     | 整数标量寄存器 |
| FREG       | 64-bit | 32     | 浮点标量寄存器 |
| PREG       | 64-bit | 64     | 整数物理寄存器 |
| VREG       | 128-bit| 32     | 向量物理寄存器 |

---

## 3. 数据结构

### 3.1 Issue Queue Entry 结构

#### 3.1.1 AIQ Entry (标量整数)

```cpp
struct AIQEntry {
    // 基本状态
    bool vld;           // 入口有效
    bool frz;           // 冻结 (等待特定条件)

    // 指令信息
    uint32_t opcode;    // 指令操作码
    uint7_t iid;        // 指令 ID (用于异常追踪)
    uint5_t pid;        // 物理指令 ID
    bool length;        // 指令长度标记

    // 控制流标记
    bool pcall;         // 过程调用标记 (CALL)
    bool rts;           // 返回指令标记 (RET)

    // 操作数就绪状态
    bool src0_vld;      // 源操作数 0 就绪
    bool src1_vld;      // 源操作数 1 就绪

    // 向量扩展状态 (RISC-V V 扩展)
    uint3_t vsew;       // 向量元素宽度
    uint3_t vlmul;      // 向量长度乘数
    uint8_t vl;         // 向量长度

    // 年龄向量 (用于仲裁)
    std::bitset<DEPTH> agevec;  // 相对年龄向量
};
```

#### 3.1.2 BIQ Entry (分支)

```cpp
struct BIQEntry {
    // 继承 AIQEntry 基本字段
    AIQEntry base;

    // 分支特殊字段
    bool pcall;         // 过程调用 (CALL)
    bool rts;           // 返回指令 (RET)
    bool length;        // 指令长度 (用于 PC 计算)
};
```

#### 3.1.3 LSIQ Entry (加载/存储)

```cpp
struct LSIQEntry {
    // 继承 AIQEntry 基本字段
    AIQEntry base;

    // 内存操作类型
    bool load;          // 加载指令标记
    bool store;         // 存储指令标记
    bool bar;           // 屏障指令
    uint2_t bar_type;   // 屏障类型
    bool no_spec;       // 禁止推测

    // 资源状态
    bool lq_full;       // Load Queue 满
    bool sq_full;       // Store Queue 满
    bool rb_full;       // Reorder Buffer 满

    // 忙/等待状态
    bool tlb_busy;      // TLB 忙
    bool wait_fence;    // 等待 FENCE
    bool wait_old;      // 等待旧指令完成

    // 异常/失败状态
    bool already_da;    // 已完成 (DA)
    bool spec_fail_set; // 推测失败
    bool unalign_2nd_set; // 未对齐第二次访问

    // 断点状态
    bool bkpta_data_set; // 断点 A 数据设置
    bool bkptb_data_set; // 断点 B 数据设置
};
```

#### 3.1.4 SDIQ Entry (存储数据)

```cpp
struct SDIQEntry {
    // 简化结构，仅检查源操作数就绪
    bool vld;           // 入口有效
    bool src0_vld;      // 源操作数 0 就绪 (地址)
    bool src1_vld;      // 源操作数 1 就绪 (数据)
    uint5_t pid;        // 物理指令 ID
};
```

### 3.2 年龄向量 (Age Vector) 机制

年龄向量用于维护 Queue 内指令的相对顺序，确保公平发射:

```cpp
class AgeVector {
private:
    static const int DEPTH = 16;  // Queue 深度
    std::bitset<DEPTH> agevec;

public:
    // 初始化年龄向量
    void initialize(const std::bitset<DEPTH>& create_agevec) {
        agevec = create_agevec;
    }

    // 检查是否比其他入口老
    bool isOlderThan(int other_idx) const {
        return agevec[other_idx];
    }

    // 清除被弹出入口对应的位
    void clearPopped(int popped_idx) {
        agevec[popped_idx] = 0;
        // 所有比 popped_idx 老的入口，对应位清零
        for (int i = 0; i < DEPTH; i++) {
            if (agevec[i] && i > popped_idx) {
                agevec[i] = 0;
            }
        }
    }

    // 获取年龄优先级 (1 的个数，越多越老)
    int getAgePriority() const {
        return agevec.count();
    }
};
```

---

## 4. 接口定义

### 4.1 输入信号

#### 4.1.1 创建/Dispatch 输入

| 信号名                                | 位宽 | 方向   | 描述                      |
|:--------------------------------------|:----:|:-------|:--------------------------|
| ctrl_ir_pipedown_inst[0:3]_vld        | 4    | Input  | IR 级指令有效 (4 条)        |
| ctrl_ir_pre_dis_*_create[0:1]_en      | 1    | Input  | 预发射创建使能 (各 IQ)      |
| ctrl_ir_pre_dis_*_create[0:1]_sel     | 2    | Input  | 预发射创建源选择           |
| aiq[0:1]_ctrl_empty/full              | 1    | Input  | AIQ 空/满状态              |
| biq_ctrl_empty/full                   | 1    | Input  | BIQ 空/满状态              |
| lsiq_ctrl_empty/full                  | 1    | Input  | LSIQ 空/满状态             |
| sdiq_ctrl_empty/full                  | 1    | Input  | SDIQ 空/满状态             |
| viq[0:1]_ctrl_empty/full              | 1    | Input  | VIQ 空/满状态              |
| lsu_idu_vmb_empty/full                | 1    | Input  | VMB 空/满状态              |

#### 4.1.2 写回/前递输入

| 信号名                                      | 位宽 | 方向   | 描述                    |
|:--------------------------------------------|:----:|:-------|:------------------------|
| iu_idu_ex2_pipe[0:1]_wb_preg_vld_dupx       | 1    | Input  | IU 写回有效              |
| iu_idu_ex2_pipe[0:1]_wb_preg_dupx           | 7    | Input  | IU 写回物理寄存器        |
| lsu_idu_wb_pipe3_wb_preg_vld_dupx           | 1    | Input  | LSU 写回有效             |
| vfpu_idu_ex1_pipe[6:7]_mfvr_inst_vld_dupx   | 1    | Input  | VFPU 标量转发有效        |

#### 4.1.3 控制信号

| 信号名                    | 位宽 | 方向   | 描述            |
|:--------------------------|:----:|:-------|:----------------|
| rtu_idu_rob_full          | 1    | Input  | ROB 满          |
| rtu_idu_flush_fe/is       | 1    | Input  | 前端/后端刷新   |
| rtu_yy_xx_flush           | 1    | Input  | 全局刷新        |
| iu_yy_xx_cancel           | 1    | Input  | 执行单元取消    |

### 4.2 输出信号

| 信号名                          | 位宽 | 方向   | 描述                    |
|:--------------------------------|:----:|:-------|:------------------------|
| ctrl_aiq[0:1]_create[0:1]_en    | 1    | Output | AIQ 创建使能              |
| ctrl_biq_create[0:1]_en         | 1    | Output | BIQ 创建使能              |
| ctrl_lsiq_create[0:1]_en        | 1    | Output | LSIQ 创建使能             |
| ctrl_sdiq_create[0:1]_en        | 1    | Output | SDIQ 创建使能             |
| ctrl_viq[0:1]_create[0:1]_en    | 1    | Output | VIQ 创建使能              |
| ctrl_dp_is_dis_*_sel            | 1-3  | Output | 数据通路发射选择          |
| ctrl_top_is_iq_full             | 1    | Output | Issue Queue 全满          |
| ctrl_is_stall                   | 1    | Output | IS 级停顿                 |
| ctrl_is_dis_stall               | 1    | Output | 发射停顿                  |
| idu_had_iq_empty                | 1    | Output | 所有 IQ 空 (调试)          |
| idu_iu_is_pcfifo_inst_vld/num   | 1/3  | Output | PCFIFO 指令有效/数量      |

---

## 5. 操作流程

### 5.1 指令创建流程

```
┌─────────────────────────────────────────────────────────────────┐
│                    指令创建流程图                                │
│                                                                  │
│     IR 阶段 (Pipedown)                                          │
│            │                                                    │
│            ▼                                                    │
│     ┌─────────────────┐                                         │
│     │ 预发射选择       │ (Pre-dispatch Sel)                      │
│     │ (Pre-dispatch)  │                                         │
│     └────────┬────────┘                                         │
│              │                                                  │
│              ▼                                                  │
│     ┌─────────────────┐                                         │
│     │ IS 级锁存        │ (IS_LATCH)                              │
│     └────────┬────────┘                                         │
│              │                                                  │
│              ▼                                                  │
│     ┌─────────────────┐                                         │
│     │ Issue Queue     │ (Entry 写入)                              │
│     │ Entry 写入       │                                         │
│     └────────┬────────┘                                         │
│              │                                                  │
│              ▼                                                  │
│     ┌─────────────────┐                                         │
│     │ 年龄向量更新     │ (Agevec Update)                         │
│     └─────────────────┘                                         │
└─────────────────────────────────────────────────────────────────┘
```

### 5.2 发射仲裁流程

```cpp
void IssueArbitration() {
    // 周期开始
    for (auto& iq : issueQueues) {
        // 1. 就绪检查 (RDY Check)
        for (auto& entry : iq.entries) {
            entry.rdy = entry.vld &&
                        entry.src0_vld &&
                        entry.src1_vld &&
                        !entry.frz &&
                        ext_rdy(entry);  // 外部条件
        }

        // 2. 年龄向量比较 (Agevec Compare)
        std::vector<int> ready_indices;
        for (int i = 0; i < iq.depth; i++) {
            if (iq.entries[i].rdy) {
                ready_indices.push_back(i);
            }
        }

        // 3. 仲裁选择 (Arbitration)
        // 选择最老的 N 条指令 (N=发射宽度)
        std::sort(ready_indices.begin(), ready_indices.end(),
            [&iq](int a, int b) {
                return iq.entries[a].agevec.count() >
                       iq.entries[b].agevec.count();
            });

        // 4. 发射使能 (Issue En)
        for (int i = 0; i < iq.issueWidth && i < ready_indices.size(); i++) {
            iq.issue_en[i] = true;
        }

        // 5. 入口弹出 (Pop Entry)
        for (int i = 0; i < iq.issueWidth; i++) {
            if (iq.issue_en[i]) {
                iq.entries[ready_indices[i]].vld = false;
                // 更新其他入口的年龄向量
                for (int j = 0; j < iq.depth; j++) {
                    if (j != ready_indices[i]) {
                        iq.entries[j].agevec[ready_indices[i]] = 0;
                    }
                }
            }
        }
    }
}
```

### 5.3 就绪检查逻辑

```cpp
// 就绪条件 = src0_vld && src1_vld && !frz && 外部条件
bool Entry::isReady() {
    return vld && src0_vld && src1_vld && !frz && extRdy();
}

// 外部就绪条件 (ext_rdy):
bool AIQEntry::extRdy() {
    return true;  // AIQ 始终就绪
}

bool BIQEntry::extRdy() {
    return true;  // BIQ 始终就绪
}

bool LSIQEntry::extRdy() {
    return !lq_full && !sq_full && !tlb_busy && !wait_fence;
}

bool SDIQEntry::extRdy() {
    return true;  // SDIQ 始终就绪
}

bool VIQEntry::extRdy() {
    // 依赖 VFPU 状态
    return !vfpu_busy;
}
```

### 5.4 前递与唤醒机制

#### 5.4.1 写回前递路径

| 来源                    | 目标         | 延迟   | 信号                           |
|:------------------------|:-------------|:------:|:-------------------------------|
| IU Pipe0 EX2            | AIQ          | 1 周期 | iu_idu_ex2_pipe0_wb_preg_*     |
| IU Pipe1 EX2            | AIQ          | 1 周期 | iu_idu_ex2_pipe1_wb_preg_*     |
| IU Pipe2 EX2            | BIQ          | 1 周期 | iu_idu_ex2_pipe2_wb_preg_*     |
| LSU Pipe3 WB            | LSIQ/AIQ     | 1 周期 | lsu_idu_wb_pipe3_wb_preg_*     |
| VFPU Pipe6 EX1          | VIQ/AIQ      | 1 周期 | vfpu_idu_ex1_pipe6_mfvr_*      |
| VFPU Pipe7 EX1          | VIQ/AIQ      | 1 周期 | vfpu_idu_ex1_pipe7_mfvr_*      |

#### 5.4.2 前递比较逻辑

```cpp
// 检查写回寄存器是否匹配入口的源寄存器
bool checkForward(const uint7_t& wb_preg, const uint7_t& src_preg, bool wb_vld) {
    return (wb_preg == src_preg) && wb_vld;
}

// 在 Entry 中应用前递
void Entry::applyForward(const std::vector<ForwardData>& forwards) {
    for (const auto& fwd : forwards) {
        if (fwd.vld) {
            if (fwd.preg == src0_preg) {
                src0_vld = true;
            }
            if (fwd.preg == src1_preg) {
                src1_vld = true;
            }
        }
    }
}
```

---

## 6. 状态机定义

### 6.1 Issue Queue Entry 状态机

```
┌─────────────────────────────────────────────────────────────┐
│                 IQ Entry 状态转换图                          │
│                                                              │
│     ┌──────────┐                                             │
│     │ INVALID  │                                             │
│     └────┬─────┘                                             │
│          │ create_en                                         │
│          ▼                                                   │
│     ┌──────────┐   src_vld    ┌──────────┐                  │
│     │ WAITING  │ ────────────→│  READY   │                  │
│     └────┬─────┘              └────┬─────┘                  │
│          │                         │                         │
│          │ flush                   │ issue_en                │
│          ▼                         ▼                         │
│     ┌──────────┐              ┌──────────┐                  │
│     │ FLUSHING │              │ ISSUING  │                  │
│     └────┬─────┘              └────┬─────┘                  │
│          │                         │                         │
│          │ vld=false               │ pop                     │
│          ▼                         ▼                         │
│     ┌──────────┐              ┌──────────┐                  │
│     │ INVALID  │              │ INVALID  │                  │
│     └──────────┘              └──────────┘                  │
└─────────────────────────────────────────────────────────────┘
```

### 6.2 停顿状态

| 停顿类型 | 信号 | 原因 |
|:---------|:-----|:-----|
| ROB Full | ctrl_is_rob_full | 退休队列满 |
| IQ Full | ctrl_is_iq_full | Issue Queue 满 |
| VMB Full | ctrl_is_vmb_full | 向量存储缓冲满 |
| Type Stall | is_dis_type_stall | 指令类型停顿 (barrier) |
| Dis Stall | ctrl_is_dis_stall | 发射停顿 |
| IS Stall | ctrl_is_stall | IS 级总停顿 |

### 6.3 停顿生成逻辑

```cpp
// Dispatch 停顿 = ROB 满 || IQ 满 || VMB 满
bool ctrl_is_dis_stall = ctrl_is_rob_full ||
                         ctrl_is_iq_full ||
                         ctrl_is_vmb_full;

// IS 停顿 = Dispatch 停顿 || Type Stall
bool ctrl_is_stall = ctrl_is_dis_stall || is_dis_type_stall;
```

### 6.4 刷新机制

| 刷新类型 | 信号 | 效果 |
|:---------|:-----|:-----|
| 前端刷新 | rtu_idu_flush_fe | 清空 IR/IS/RF 所有指令 |
| 后端刷新 | rtu_idu_flush_is | 清空 IS/RF 指令 |
| 全局刷新 | rtu_yy_xx_flush | 清空整个流水线 |
| 取消 | iu_yy_xx_cancel | 取消当前发射指令 |

---

## 7. 时序规范

### 7.1 流水线级数

| 级名 | 功能 | 延迟 |
|:-----|:-----|:----:|
| IB (Instruction Buffer) | 指令缓冲、寄存器重命名分配 | 1 周期 |
| RF (Register File Read) | 寄存器文件读、操作数转发 | 1 周期 |
| IS (Issue) | Issue Queue 缓存、发射仲裁 | 1+ 周期 |

### 7.2 IS 级详细时序

```
周期 T0: 指令写入 Issue Queue Entry (Create)
周期 T1+: 就绪检查 (RDY Check)、年龄向量更新
周期 Tn: 仲裁选中、发射 (Issue)
```

### 7.3 关键路径

| 路径 | 描述 | 优化方法 |
|:-----|:-----|:---------|
| 写回→就绪检查 | wb_preg_vld → x_rdy | 提前比较、时钟借贷 |
| 就绪→仲裁 | x_rdy → x_issue_en | 预仲裁、年龄向量提前计算 |
| 仲裁→发射 | x_issue_en → ctrl_*_create_en | 流水线发射选择 |

### 7.4 时钟门控

| 信号 | 条件 |
|:-----|:-----|
| is_inst_clk_en | ctrl_ir_pipedown_gateclk \|\| is_inst[0:3]_vld |
| queue_full_clk_en | ctrl_ir_pipedown_gateclk \|\| is_inst_vld \|\| iq_full_updt |
| ctrl_*_create_gateclk_en | is_dis_*_create_en |

---

## 8. 配置参数

### 8.1 编译时配置

在 gem5 中可以通过 Python 配置参数实现:

```python
# C910 默认配置参数
class C910IQParams:
    # 发射宽度
    IDU_ISSUE_WIDTH = 8       # 总发射宽度

    # Queue 深度
    IDU_AIQ_DEPTH = 11        # AIQ 深度
    IDU_BIQ_DEPTH = 12        # BIQ 深度
    IDU_LSIQ_DEPTH = 16       # LSIQ 深度
    IDU_SDIQ_DEPTH = 8        # SDIQ 深度
    IDU_VIQ_DEPTH = 10        # VIQ 深度
    IDU_VMB_DEPTH = 8         # VMB 深度
```

### 8.2 CSR 控制

| CSR | 功能 | 影响 |
|:----|:-----|:-----|
| mstatus.IQ_BYPASS_DISABLE | IQ 旁路禁用 | 禁用 IQ 优化路径 |
| mstatus.SRC2_FWD_DISABLE | SRC2 前递禁用 | 禁用源操作数 2 前递 |
| mstatus.SRCV2_FWD_DISABLE | SRCV2 前递禁用 | 禁用向量源操作数 2 前递 |

---

## 9. 实现建议

### 9.1 gem5 O3 IQ 修改建议

为了支持 C910 风格的多 Queue 架构，建议对 gem5 O3 CPU 的 InstructionQueue 进行以下修改:

#### 9.1.1 增加多 Queue 支持

```cpp
// inst_queue.hh
class InstructionQueue {
  private:
    // 原有的单一 IQ 改为多 IQ 数组
    std::vector<IQUnit*> iqs;  // 多 IQ 单位

    // 按指令类型路由到不同 IQ
    IQUnit* routeToIQ(const DynInstPtr& inst);
};
```

#### 9.1.2 增加年龄向量机制

```cpp
// dyn_inst.hh
class DynInst {
  private:
    // 年龄向量用于跨 Queue 仲裁
    std::bitset<MAX_IQ_DEPTH> ageVector;

    // 获取年龄优先级
    int getAgePriority() const { return ageVector.count(); }

    // 更新年龄向量
    void updateAgeVector(const DynInstPtr& older_inst);
};
```

#### 9.1.3 增加快速前递路径

```cpp
// inst_queue.hh
class InstructionQueue {
  private:
    // 前递数据结构
    struct ForwardData {
        PhysRegId preg;
        bool vld;
        int value;  // 可选：直接前递数据
    };

    // 写回前递端口
    std::vector<ForwardData> wbForwards[MaxPipeCount];

    // 检查并应用前递
    void checkAndApplyForward(const DynInstPtr& inst);
};
```

### 9.2 性能优化建议

1. **预仲裁**: 在周期前半段就进行年龄比较，减少关键路径
2. **年龄向量优化**: 使用计数器替代位向量，减少比较逻辑
3. **时钟门控**: 对空闲的 Queue Entry 进行时钟门控，降低功耗
4. **流水线发射**: 将仲裁逻辑分为多级流水线，支持更高频率

### 9.3 调试支持

建议增加以下调试信号:

```cpp
// 调试信号 (HAD)
struct DebugSignals {
    bool idu_had_iq_empty;        // 所有 Issue Queue 空
    bool idu_had_pipeline_empty;  // 整个流水线空
    bool idu_had_pipe_stall;      // 流水线停顿
    InstInfo idu_had_id_inst[3];  // 发射到 ID 的指令信息
    WritebackData idu_had_wb_data;// 写回数据
};

// 性能计数事件 (HPCP)
struct PerfCounters {
    Counter idu_hpcp_rf_inst_vld;           // RF 级指令有效
    Counter idu_hpcp_rf_pipe[8]_inst_vld;   // 各 Pipe 发射指令数
    Counter idu_hpcp_rf_pipe[8]_lch_fail;   // 锁存失败次数
    Counter idu_hpcp_backend_stall;         // 执行单元导致的停顿
    Counter idu_hpcp_fence_sync_vld;        // FENCE 指令执行次数
};
```

---

## 10. 文件列表

基于 C910 RTL 实现，以下是关键文件列表:

| 文件名 | 功能 |
|:-------|:-----|
| ct_idu_top.v | IDU 顶层模块 |
| ct_idu_is_ctrl.v | Issue 控制逻辑 |
| ct_idu_is_dp.v | Issue 数据通路 |
| ct_idu_is_aiq0.v / ct_idu_is_aiq0_entry.v | ALU Issue Queue 0 |
| ct_idu_is_aiq1.v | ALU Issue Queue 1 |
| ct_idu_is_biq.v / ct_idu_is_biq_entry.v | Branch Issue Queue |
| ct_idu_is_lsiq.v / ct_idu_is_lsiq_entry.v | Load/Store Issue Queue |
| ct_idu_is_sdiq.v / ct_idu_is_sdiq_entry.v | Store Data Issue Queue |
| ct_idu_is_viq0.v | Vector Issue Queue 0 |
| ct_idu_is_viq1.v | Vector Issue Queue 1 |
| ct_idu_dep_reg_entry.v | 依赖寄存器 Entry |
| ct_idu_dep_vreg_entry.v | 依赖向量寄存器 Entry |

---

## 11. 总结

C910 Issue Queue 是实现乱序执行的核心组件，具有以下特点:

1. **多 Queue 并行**: 7 个独立 Issue Queue 支持指令分类缓存
2. **宽发射**: 每周期最多 8 条指令发射到 8 个执行 Pipe
3. **动态就绪**: 实时检查操作数就绪和外部条件
4. **年龄仲裁**: 基于年龄向量的公平仲裁机制
5. **快速前递**: 多写回端口支持快速数据前递
6. **灵活停顿**: 多种停顿源确保正确性
7. **完善调试**: HAD/HPCP 接口支持调试和性能分析

本规格文档旨在将 C910 的 Issue Queue 设计理念移植到 gem5 O3 CPU 中，为 RISC-V 处理器建模提供参考。

---

**文档版本**: 1.0
**生成日期**: 2026-04-08
**基于 RTL 版本**: REV=1, SUB_VER=4, PATCH=3
**gem5 目标分支**: stable
