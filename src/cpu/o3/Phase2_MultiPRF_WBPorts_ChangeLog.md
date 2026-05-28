# Phase 2：多 PRF + 分类写回端口 — 修改记录

## 修改日期
2026/05/08

## 修改目标
将单一 Scoreboard + 统一 `wbWidth` 改造为 3 个独立 PRF（PREG/VREG/EREG）+ 分类写回端口，对齐 OpenC910 的多 PRF 架构。

---

## 1. `scoreboard.hh` — 多 PRF Scoreboard 改造

**核心变化**：将单一 `regScoreBoard` 拆分为 4 个独立记分板

| 成员 | 寄存器类型 | 说明 |
|------|-----------|------|
| `pregScoreBoard` | `IntRegClass` | 整数 PRF（如 95 个） |
| `eregScoreBoard` | `FloatRegClass` | 标量 FP PRF（如 32 个） |
| `vregScoreBoard` | `VecRegClass` | 向量 PRF（如 64 个） |
| `otherScoreBoard` | VecPred/VecElem/Mat/CC | 其他寄存器类 |

**接口变更**：
- 构造函数：`Scoreboard(name, numPregs, numEregs, numVregs, numOtherRegs)`（原为 `name, numPhysicalRegs`）
- 新增 accessor：`numPregs()` / `numEregs()` / `numVregs()` / `numOtherRegs()` / `totalNumPhysRegs()`
- `getReg()` / `setReg()` / `unsetReg()` 签名不变，内部按 `PhysRegId->classValue()` 路由到对应 PRF

---

## 2. `scoreboard.cc` — 构造函数适配

```cpp
// 原：regScoreBoard(_numPhysicalRegs, true)
// 新：4 个 vector 分别初始化，_numPhysRegs = 4 者之和
```

---

## 3. `BaseO3CPU.py` — 新增分类写回端口参数

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `pregWBWidth` | 3 | 整数 PRF 写回端口数 |
| `vregWBWidth` | 3 | 向量 PRF 写回端口数 |
| `eregWBWidth` | 2 | 标量 FP PRF 写回端口数 |

原 `wbWidth` 保留为 legacy fallback（默认 3）。

---

## 4. `iew.hh` — 新增每 PRF 写回宽度成员

```cpp
unsigned wbWidth;           // 总宽度上限（legacy）
unsigned pregWBWidth;       // PREG 端口数
unsigned vregWBWidth;       // VREG 端口数
unsigned eregWBWidth;       // EREG 端口数
```

---

## 5. `iew.cc` — `writebackInsts()` 多端口改造

**原逻辑**：遍历 `wbWidth` 个槽位，统一写回。

**新逻辑**：
1. 对每条待写回指令，先统计其目的寄存器涉及哪些 PRF 类型（PREG/VREG/EREG）
2. 检查各 PRF 类型的写回计数器是否已达端口上限
3. 若任一类型超出端口数 → `break`，停止本轮写回
4. 写回成功后递增对应 PRF 类型的端口计数

**端口映射对齐**：

| 端口 | 来源 | 对应 PRF |
|------|------|----------|
| PREG 端口 | IU ALU Pipe0/Pipe1, LSU Load Pipe3 | `IntRegClass` |
| VREG 端口 | VFPU Pipe6/Pipe7, LSU Load Pipe3 | `VecRegClass` |
| EREG 端口 | VFPU Pipe6/Pipe7 | `FloatRegClass` |

---

## 6. `cpu.cc` — Scoreboard 构造调用更新

**原**：
```cpp
scoreboard(name() + ".scoreboard", regFile.totalNumPhysRegs())
```

**新**：
```cpp
scoreboard(name() + ".scoreboard",
           params.numPhysIntRegs,    // PREG
           params.numPhysFloatRegs,  // EREG
           params.numPhysVecRegs,    // VREG
           regFile.totalNumPhysRegs()
               - params.numPhysIntRegs
               - params.numPhysFloatRegs
               - params.numPhysVecRegs)  // Other
```

---

## 影响范围

| 文件 | 变更类型 | 说明 |
|------|---------|------|
| `scoreboard.hh` | 大改 | 多 PRF 分离 + accessor 新增 |
| `scoreboard.cc` | 中改 | 构造函数重写 |
| `BaseO3CPU.py` | 小改 | 3 个新参数 |
| `iew.hh` | 小改 | 3 个新成员 |
| `iew.cc` | 中改 | `writebackInsts()` 端口分类 |
| `cpu.cc` | 小改 | Scoreboard 构造参数 |

**不受影响**：`rename.cc` 中的 `scoreboard->getReg/setReg/unsetReg` 调用（接口签名不变）；`inst_queue.cc` 中的内部 `regScoreboard`（独立于 Scoreboard 类）。
