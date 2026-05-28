# O3 CPU - Fetch Stage 详细分析

## 概述

Fetch 阶段负责从指令缓存 (I-cache) 中取指，将原始字节流解码为静态指令，并创建动态指令对象传递给 Decode 阶段。Fetch 阶段与 BAC (Branch and Address Calculation) 和 FTQ (Fetch Target Queue) 紧密协作。

---

## 一、Fetch 阶段架构

### 1.1 类定义 (fetch.hh)

```cpp
class Fetch
{
  public:
    enum FetchStatus {
        Active,
        Inactive
    };

    enum ThreadStatus {
        Running,
        Idle,
        Squashing,
        Blocked,
        Fetching,
        TrapPending,
        QuiescePending,
        ItlbWait,
        IcacheWaitResponse,
        IcacheWaitRetry,
        IcacheAccessComplete,
        FtqWait,
        NoGoodAddr,
        ThreadStatusMax
    };

  private:
    FetchStatus _status;
    ThreadStatus fetchStatus[MaxThreads];

    // SMT 取指策略
    SMTFetchPolicy fetchPolicy;
    std::list<ThreadID> priorityList;

    // PC 和缓冲区管理
    std::unique_ptr<PCStateBase> pc[MaxThreads];
    Addr fetchOffset[MaxThreads];
    StaticInstPtr macroop[MaxThreads];
    bool delayedCommit[MaxThreads];

    RequestPtr memReq[MaxThreads];

    // 取指缓冲区
    uint8_t *fetchBuffer[MaxThreads];
    Addr fetchBufferPC[MaxThreads];
    bool fetchBufferValid[MaxThreads];
    unsigned fetchBufferSize;
    Addr fetchBufferMask;

    // 取指队列
    std::deque<DynInstPtr> fetchQueue[MaxThreads];
    unsigned fetchQueueSize;

    // 状态标志
    bool wroteToTimeBuffer;
    int numInst;
    bool cacheBlocked;
    PacketPtr retryPkt;
    ThreadID retryTid;

    // 停顿源
    struct Stalls {
        bool decode;
        bool drain;
    };
    Stalls stalls[MaxThreads];

    // SMT 相关
    ThreadID numThreads;
    ThreadID numFetchingThreads;
    ThreadID threadFetched;
    bool interruptPending;

    // 配置参数
    const bool decoupledFrontEnd;
    const Cycles decodeToFetchDelay;
    const Cycles renameToFetchDelay;
    const Cycles iewToFetchDelay;
    const Cycles commitToFetchDelay;
    const unsigned fetchWidth;
    const unsigned decodeWidth;
    const unsigned maxFTPerCycle;
    const unsigned maxTakenPredPerCycle;

    // BAC 和 FTQ 指针
    BAC *bac;
    FTQ *ftq;
};
```

### 1.2 线程状态说明

| 状态 | 说明 |
|------|------|
| `Running` | 正常取指 |
| `Idle` | 空闲，无取指需求 |
| `Squashing` | 正在清除错误路径指令 |
| `Blocked` | 被下游阶段阻塞 |
| `Fetching` | 正在发起取指请求 |
| `TrapPending` | 等待 trap 处理 |
| `ItlbWait` | 等待 ITLB 地址翻译 |
| `IcacheWaitResponse` | 等待 I-cache 响应 |
| `IcacheWaitRetry` | I-cache 繁忙，等待重试 |
| `FtqWait` | 等待 FTQ 提供取指目标 |

---

## 二、tick() 主函数

### 2.1 执行流程

```cpp
void Fetch::tick()
{
    bool activity = false;
    bool status_change = false;

    for (const ThreadID tid : *activeThreads) {
        // 1. 检查来自下游阶段的信号，更新线程状态
        status_change = status_change || checkSignalsAndUpdate(tid);

        // 2. 执行取指
        fetch(status_change);
    }

    // 3. 更新总体 Fetch 状态
    if (status_change) {
        updateFetchStatus();
    }

    // 4. 标记本周期是否有活动
    if (wroteToTimeBuffer) {
        activity = true;
        wroteToTimeBuffer = false;
    }

    if (activity) {
        cpu->activityThisCycle();
    }
}
```

### 2.2 checkSignalsAndUpdate() - 信号检查

Fetch 阶段接收来自多个阶段的反向信号：

- **来自 Commit**: squash 信号（分支预测错误）、trap 信号
- **来自 IEW**: 资源不足导致的阻塞信号
- **来自 Decode**: 反压信号（Decode 队列满）
- **来自 BAC/FTQ**: FTQ 满或空的信号

当收到 squash 信号时：
1. 清空取指队列和滑移缓冲区
2. 将 fetchStatus 设置为 `Squashing`
3. 更新 PC 到 squash 后的正确地址

---

## 三、核心取指逻辑

### 3.1 fetch() 函数

```cpp
void Fetch::fetch(bool &status_change)
{
    if (fetchStatus[tid] == Running) {
        DPRINTF(Fetch, "[tid:%i] Fetching instructions.\n", tid);

        numInst = 0;

        // 取指循环，受 fetchWidth 限制
        while (numInst < fetchWidth) {
            // 解耦前端模式 - 从 FTQ 读取取指目标
            if (decoupledFrontEnd) {
                FetchTargetPtr ft = ftq->readHead(tid);
                if (!ft || !ftq->isReady(tid)) {
                    break;  // FTQ 为空或锁定
                }
            }

            // 检查是否正在等待 I-cache 响应
            if (memReq[tid]) {
                if (issuePipelinedIfetch[tid]) {
                    pipelineIcacheAccesses(tid);
                }
                break;
            }

            TheISA::PCState this_pc = *pc[tid];

            // 从取指缓冲区读取指令
            if (!fetchBufferValid[tid]) {
                if (!fetchCacheLine(this_pc.instAddr(), tid, this_pc.instAddr())) {
                    break;
                }
            }

            // 预译码指令
            InstDecoder *decoder = this->decoder[tid];
            Addr fetch_PC = this_pc.instAddr();
            MicroPC micro_pc = this_pc.microPC();

            StaticInstPtr inst = nullptr;
            if (fetchBufferValid[tid]) {
                uint8_t *inst_data = &fetchBuffer[tid][fetchOffset[tid]];
                inst = decoder->decodeInst(
                    fetch_pc, micro_pc, inst_data,
                    fetchBufferSize - fetchOffset[tid]);
            }

            if (inst != nullptr) {
                // 创建动态指令
                DynInstPtr dyn_inst =
                    buildInst(tid, inst, macroop[tid], this_pc, this_pc, false);

                // 更新 PC
                if (decoupledFrontEnd) {
                    if (bac->updatePC(dyn_inst, *pc[tid], ftq->readHead(tid))) {
                        ftq->popHead(tid);  // 分支 taken
                    }
                    if (!ftq->readHead(tid) ||
                        ftq->readHead(tid)->isExitInst(pc[tid]->instAddr())) {
                        ftq->popHead(tid);
                    }
                } else {
                    inst->advancePC(*pc[tid]);
                }

                // 添加到取指队列
                fetchQueue[tid].push_back(dyn_inst);
                wroteToTimeBuffer = true;
                numInst++;

                fetchOffset[tid] += inst->size();
                macroop[tid] = nullptr;

                if (fetchOffset[tid] >= fetchBufferSize ||
                    fetchOffset[tid] >= cacheBlkSize) {
                    fetchBufferValid[tid] = false;
                }

                if (inst->isMacroop()) {
                    macroop[tid] = inst;
                }
            } else {
                // 预译码失败
                fetchBufferValid[tid] = false;
                if (!fetchCacheLine(this_pc.instAddr(), tid, this_pc.instAddr())) {
                    break;
                }
            }
        }

        // 写入时间缓冲，发送到 Decode
        if (!fetchQueue[tid].empty()) {
            toDecode->fetchInfo[tid].size = fetchQueue[tid].size();
            toDecode->fetchInfo[tid].tid = tid;
            for (int i = 0; i < fetchQueue[tid].size(); i++) {
                toDecode->fetchInfo[tid].insts[i] = fetchQueue[tid][i];
            }
            wroteToTimeBuffer = true;
        }

        fetchQueue[tid].clear();

        if (numInst == 0 && decoupledFrontEnd && fetchStatus[tid] == Running) {
            if (!ftq->isReady(tid)) {
                fetchStatus[tid] = FtqWait;
                status_change = true;
            }
        }
    }
}
```

### 3.2 取指流程总结

```
┌─────────────────────────────────────────────────────────────┐
│                      Fetch Stage                             │
│                                                              │
│  1. checkSignalsAndUpdate() ── 检查 squash/backpressure     │
│                                                              │
│  2. fetch() 循环:                                           │
│     ┌──────────────────────────────────────────────────┐    │
│     │ a. 从 FTQ 获取取指目标 (解耦模式)                  │    │
│     │ b. 检查 fetchBuffer 是否有效                     │    │
│     │ c. 如无效 → fetchCacheLine() 取 cache line       │    │
│     │ d. decoder->decodeInst() 预译码                   │    │
│     │ e. buildInst() 创建 DynInst                      │    │
│     │ f. 更新 PC (BAC.updatePC 或 advancePC)            │    │
│     │ g. 推入 fetchQueue                               │    │
│     └──────────────────────────────────────────────────┘    │
│                                                              │
│  3. 写入 timeBuffer → toDecode                              │
│                                                              │
└─────────────────────────────────────────────────────────────┘
```

---

## 四、I-cache 访问

### 4.1 fetchCacheLine() - 发起取指请求

```cpp
bool Fetch::fetchCacheLine(Addr vaddr, ThreadID tid, Addr pc)
{
    // 创建内存请求
    RequestPtr req = std::make_shared<Request>();
    req->setVirt(vaddr, cacheBlkSize, Request::INST_FETCH, 0,
                cpu->getPid(tid), pc);

    memReq[tid] = req;
    fetchOffset[tid] = vaddr - fetchBufferAlignPC(vaddr);
    fetchBufferPC[tid] = vaddr;

    // 发起 TLB 翻译
    Translation *translation = new FetchTranslation(this);
    cpu->mmu->translateTiming(
        translation, cpu->tcBase(tid), req, &icachePort);

    return true;
}
```

### 4.2 IcachePort - 缓存端口

```cpp
class IcachePort : public RequestPort
{
  protected:
    Fetch *fetch;
  public:
    IcachePort(Fetch *_fetch, CPU *_cpu);
    virtual bool recvTimingResp(PacketPtr pkt);
    virtual void recvReqRetry();
};
```

- **recvTimingResp()**: 接收 I-cache 响应，将数据填入 fetchBuffer
- **recvReqRetry()**: I-cache 繁忙时收到重试信号

### 4.3 FetchTranslation - TLB 翻译

```cpp
class FetchTranslation : public BaseMMU::Translation
{
  protected:
    Fetch *fetch;
  public:
    FetchTranslation(Fetch *_fetch) : fetch(_fetch) {}
    void markDelayed() {}
    void finish(const Fault &fault, const RequestPtr &req,
        gem5::ThreadContext *tc, BaseMMU::Mode mode);
};
```

翻译完成后：
1. 如果翻译失败 → 产生 trap 或等待页面错误
2. 如果翻译成功 → 通过 icachePort 发送 I-cache 请求

---

## 五、SMT 取指策略

### 5.1 策略类型

```cpp
enum class SMTFetchPolicy {
    RoundRobin,    // 轮询
    IQCount,       // 根据 IQ 空闲条目数分配
    LSQCount,      // 根据 LSQ 空闲条目数分配
    BranchCount,   // 根据分支未预测数分配
};
```

### 5.2 RoundRobin 实现

```cpp
ThreadID Fetch::roundRobin()
{
    for (auto it = priorityList.begin(); it != priorityList.end(); ++it) {
        if (fetchStatus[*it] == Running) {
            ThreadID tid = *it;
            priorityList.erase(it);
            priorityList.push_back(tid);
            return tid;
        }
    }
    return -1;
}
```

---

## 六、BAC 阶段 - 分支地址计算

### 6.1 BAC 职责

BAC (Branch and Address Calculation) 阶段负责:
1. **分支预测**: 管理 BPredUnit 分支预测器
2. **取指目标生成**: 生成取指目标并插入 FTQ
3. **PC 更新**: 计算下一条指令的 PC

### 6.2 BAC 两种模式

| 模式 | 说明 |
|------|------|
| **耦合前端** | BAC 与 Fetch 同步，每条指令预译码时预测 |
| **解耦前端** | BAC 独立于 Fetch 运行，预先生成取指目标 |

### 6.3 generateFetchTargets() - 解耦前端核心

```cpp
void BAC::generateFetchTargets(ThreadID tid, bool &status_change)
{
    PCStateBase &cur_pc = *bacPC[tid];
    int num_ft = 0;
    int num_taken = 0;

    while ((num_ft < maxFTPerCycle) && (num_taken < maxTakenPredPerCycle)) {
        Addr search_addr = cur_pc.instAddr();
        Addr start_addr = search_addr;

        FetchTargetPtr curFT = newFetchTarget(tid, cur_pc);
        num_ft++;

        bool branch_found = false;

        // BTB 搜索
        while (true) {
            branch_found = bpu->BTBValid(tid, search_addr);
            if (branch_found) break;
            if ((search_addr - start_addr) >= fetchTargetWidth) break;
            search_addr += minInstSize;
        }

        cur_pc.set(search_addr);
        std::unique_ptr<PCStateBase> next_pc(cur_pc.clone());
        StaticInstPtr staticInst = nullptr;

        if (branch_found) {
            staticInst = bpu->BTBGetInst(tid, cur_pc.instAddr());
            predict_taken = predict(tid, staticInst, curFT, *next_pc);
            stats.branches++;
            if (predict_taken) num_taken++;
        }

        if (!predict_taken) {
            next_pc->set(cur_pc.instAddr() + minInstSize);
        }

        curFT->finalize(cur_pc, branch_found, predict_taken, *next_pc);
        ftq->insert(tid, curFT);

        set(cur_pc, *next_pc);

        if (ftq->isFull(tid)) {
            bacStatus[tid] = FTQFull;
            status_change = true;
            break;
        }
    }
}
```

---

## 七、FTQ - 取指目标队列

### 7.1 FetchTarget 结构

```cpp
class FetchTarget
{
    ThreadID tid;
    std::unique_ptr<PCStateBase> startPC;
    std::unique_ptr<PCStateBase> endPC;
    std::unique_ptr<PCStateBase> predTarg;   // 预测分支目标
    FTSeqNum ftNum;
    bool foundBranch;
    bool predTaken;
    BPredUnit::PredictorHistory *bpuHistory;
};
```

### 7.2 FTQ 操作

| 操作 | 说明 |
|------|------|
| `insert(tid, ft)` | 插入取指目标到队列 |
| `readHead(tid)` | 读取队首元素 |
| `popHead(tid)` | 弹出队首元素 |
| `squash(tid)` | squash 时清空队列 |
| `lock(tid)` / `unlock(tid)` | 锁定/解锁队列 |
| `isReady(tid)` | 检查是否可消费 |
| `isFull(tid)` | 检查队列是否满 |

---

## 八、阶段间通信

### 8.1 输出到 Decode

```cpp
struct FetchStruct {
    struct {
        DynInstPtr insts[MaxFetchWidth];
        int size;
        ThreadID tid;
    } fetchInfo[MaxThreads];
};
```

Fetch 将指令打包到 `toDecode->fetchInfo`，通过 `fetchQueue` 时间缓冲传递给 Decode。

### 8.2 接收反向信号

Fetch 从以下阶段接收信号：
- **Decode**: 反压（Decode 队列满）
- **Rename**: 反压
- **IEW**: 资源不足阻塞
- **Commit**: squash 和 trap 信号

---

## 九、关键参数

| 参数 | 说明 |
|------|------|
| `fetchWidth` | 每周期最大取指数 |
| `fetchBufferSize` | 取指缓冲区大小 |
| `fetchQueueSize` | 取指队列深度 |
| `decoupledFrontEnd` | 是否启用解耦前端 |
| `maxFTPerCycle` | 每周期最大 FT 生成数 |
| `maxTakenPredPerCycle` | 每周期最大 taken 预测数 |
| `fetchTargetWidth` | 取指目标搜索宽度 |

---

## 生成信息

- **分析范围**: Fetch Stage (fetch.hh/cc, bac.hh/cc, ftq.hh/cc)
- **gem5 版本**: v25.1.0.0
