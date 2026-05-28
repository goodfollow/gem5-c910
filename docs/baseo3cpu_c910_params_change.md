# BaseO3CPU 参数修改 — 对齐 OpenC910

修改文件: `src/cpu/o3/BaseO3CPU.py`

## 修改汇总

| 参数 | 原值 | 新值 | C910 RTL 依据 |
|------|------|------|---------------|
| fetchWidth | 8 | 3 | ct_ifu_top.v:382-386 |
| decodeWidth | 8 | 3 | ct_idu_top.v:824-828 |
| renameWidth | 8 | 4 | ct_idu_ir_dp.v:123-155 |
| dispatchWidth | 8 | 4 | ct_rtu_rob.v:31-46 |
| wbWidth | 8 | 3 | ct_rtu_pst_preg.v:6132-6134 |
| commitWidth | 8 | 3 | ct_rtu_rob.v:449-451 |
| LQEntries | 32 | 16 | ct_lsu_lq.v:159 |
| SQEntries | 32 | 12 | ct_lsu_sq_entry.v:614 |
| numPhysIntRegs | 256 | 95 | ct_idu_rf_prf_pregfile.v |
| numPhysFloatRegs | 256 | 32 | ct_idu_rf_prf_eregfile.v |
| numPhysVecRegs | 256 | 64 | ct_idu_rf_prf_vregfile.v |
| numROBEntries | 192 | 64 | ct_rtu_rob.v |

## Diff

```diff
@@ -83,7 +83,7 @@ class BaseO3CPU(BaseCPU):
     renameToFetchDelay = Param.Cycles(1, "Rename to fetch delay")
     iewToFetchDelay = Param.Cycles(1, "Issue/Execute/Writeback to fetch delay")
     commitToFetchDelay = Param.Cycles(1, "Commit to fetch delay")
-    fetchWidth = Param.Unsigned(8, "Fetch width")
+    fetchWidth = Param.Unsigned(3, "Fetch width")
     fetchBufferSize = Param.Unsigned(64, "Fetch buffer size in bytes")
     fetchQueueSize = Param.Unsigned(
         32, "Fetch queue size in micro-ops per-thread"

@@ -98,14 +98,14 @@ class BaseO3CPU(BaseCPU):
     # Forward pipeline delays
     bacToFetchDelay = Param.Cycles(1, "Branch address calc. to fetch delay")
     fetchToDecodeDelay = Param.Cycles(1, "Fetch to decode delay")
-    decodeWidth = Param.Unsigned(8, "Decode width")
+    decodeWidth = Param.Unsigned(3, "Decode width")

     iewToRenameDelay = Param.Cycles(
         1, "Issue/Execute/Writeback to rename delay"
     )
     commitToRenameDelay = Param.Cycles(1, "Commit to rename delay")
     decodeToRenameDelay = Param.Cycles(1, "Decode to rename delay")
-    renameWidth = Param.Unsigned(8, "Rename width")
+    renameWidth = Param.Unsigned(4, "Rename width")

@@ -116,15 +116,15 @@ class BaseO3CPU(BaseCPU):
     issueToExecuteDelay = Param.Cycles(
         1, "Issue to execute delay (internal to the IEW stage)"
     )
-    dispatchWidth = Param.Unsigned(8, "Dispatch width")
+    dispatchWidth = Param.Unsigned(4, "Dispatch width")
     issueWidth = Param.Unsigned(8, "Issue width")
-    wbWidth = Param.Unsigned(8, "Writeback width")
+    wbWidth = Param.Unsigned(3, "Writeback width")

     iewToCommitDelay = Param.Cycles(
         1, "Issue/Execute/Writeback to commit delay"
     )
     renameToROBDelay = Param.Cycles(1, "Rename to reorder buffer delay")
-    commitWidth = Param.Unsigned(8, "Commit width")
+    commitWidth = Param.Unsigned(3, "Commit width")

@@ -139,8 +139,8 @@ class BaseO3CPU(BaseCPU):
         5, "Time buffer size for forward communication"
     )

-    LQEntries = Param.Unsigned(32, "Number of load queue entries")
-    SQEntries = Param.Unsigned(32, "Number of store queue entries")
+    LQEntries = Param.Unsigned(16, "Number of load queue entries")
+    SQEntries = Param.Unsigned(12, "Number of store queue entries")
     LSQDepCheckShift = Param.Unsigned(
         4, "Number of places to shift addr before check"
     )

@@ -172,12 +172,12 @@ class BaseO3CPU(BaseCPU):
     numRobs = Param.Unsigned(1, "Number of Reorder Buffers")

     numPhysIntRegs = Param.Unsigned(
-        256, "Number of physical integer registers"
+        95, "Number of physical integer registers"
     )
     numPhysFloatRegs = Param.Unsigned(
-        256, "Number of physical floating point registers"
+        32, "Number of physical floating point registers"
     )
-    numPhysVecRegs = Param.Unsigned(256, "Number of physical vector registers")
+    numPhysVecRegs = Param.Unsigned(64, "Number of physical vector registers")
     numPhysVecPredRegs = Param.Unsigned(
         32, "Number of physical predicate registers"
     )

@@ -185,7 +185,7 @@ class BaseO3CPU(BaseCPU):
     # most ISAs don't use condition-code regs, so default is 0
     numPhysCCRegs = Param.Unsigned(0, "Number of physical cc registers")
     instQueues = VectorParam.IQUnit(IQUnit(), "Vector of IQs")
-    numROBEntries = Param.Unsigned(192, "Number of reorder buffer entries")
+    numROBEntries = Param.Unsigned(64, "Number of reorder buffer entries")
```

## 参数分类

### 流水线宽度 (6 项)
- **fetchWidth** 8→3：取指宽度，受 IFU 每周期取指能力限制
- **decodeWidth** 8→3：译码宽度，与取指匹配
- **renameWidth** 8→4：重命名宽度，略大于译码以支持多发射
- **dispatchWidth** 8→4：分发宽度，与重命名匹配
- **wbWidth** 8→3：写回宽度，与 commit 匹配
- **commitWidth** 8→3：提交宽度，ROB 每周期可提交指令数

### 队列大小 (3 项)
- **LQEntries** 32→16：Load Queue 条目数
- **SQEntries** 32→12：Store Queue 条目数
- **numROBEntries** 192→64：ROB 总条目数

### 物理寄存器 (3 项)
- **numPhysIntRegs** 256→95：整型物理寄存器数
- **numPhysFloatRegs** 256→32：浮点物理寄存器数
- **numPhysVecRegs** 256→64：向量物理寄存器数

### 保持不变
- **issueWidth** 8：C910 issueWidth 为 8，与原值一致
- **squashWidth**：RTL 中不存在此概念，保持未设置
- **numPhysVecPredRegs** / **numPhysMatRegs** / **numPhysCCRegs**：RTL 中不存在，保持原值
