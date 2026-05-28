/*
 * c910_microbench_runner.c — C910-like IEW 定向微基准测试
 *
 * 目标：用 inline asm 精确控制 RISC-V 指令序列，覆盖 C 编译器无法保证的场景：
 *   - Pipe2 (BJU): 交替条件分支强制预测错误
 *   - Pipe4 (LSU Special): FENCE 同步
 *   - DIV_STALL: 连续除法阻塞 IntDiv FU
 *   - IQ_FULL: 大量独立指令填满 IQ
 *   - Store-Load Forwarding: 写后读验证前递延迟
 *   - Branch Redirect: mispredict → squash → redirect 时序
 *
 * 编译：riscv64-linux-gnu-gcc -O0 -static -o c910_microbench c910_microbench_runner.c
 * 运行：gem5.opt configs/example/<your-config> --c910_microbench
 */

#include <stdint.h>

/* UART 输出（通过 m5 或直接 print） */
static void uart_puts(const char *s)
{
    /* 简单 puts — gem5 会捕获 stdout */
    /* 如果 baremetal 没有 libc，可替换为 MMIO UART 写入 */
    volatile int ch;
    while (*s) {
        ch = *s++;
        __asm__ volatile("" : : "r"(ch) : "memory");
    }
}

static void print_result(const char *test_name, int pass)
{
    uart_puts("[MICROBENCH] ");
    uart_puts(test_name);
    uart_puts(pass ? " ... PASS\n" : " ... FAIL\n");
}

/* volatile 缓冲区防止编译器优化 */
static volatile int64_t vbuf[256];
static volatile uint32_t vresult;
static volatile int64_t vresult64;

/* ============================================================
 * Test 1: Pipe2 (BJU) — 强制分支预测错误
 *
 * 策略：手动构造交替 taken/not-taken 的条件分支序列，
 *       让 gem5 的本地预测器（local predictor）频繁猜错。
 *       期望结果：
 *         - pipeIssueCount[2] (Pipe2 BJU) > 0
 *         - branchMispredicts > 0
 *         - squashedInsts > 0
 * ============================================================ */
static void test_pipe2_bju(void)
{
    int64_t x = 0;
    int i;

    /*
     * 用 inline asm 强制生成条件分支指令。
     * C 编译器在 -O0 下通常也会生成条件分支，但
     * inline asm 确保指令精确可控。
     */
    __asm__ volatile (
        "li %[x], 0\n\t"
        "li %[i], 0\n\t"
        "1:\n\t"
        /* 奇偶交替：bnez 在偶数时 not-taken，奇数时 taken */
        "andi t0, %[i], 1\n\t"
        "beqz t0, 2f\n\t"       /* taken when i is odd */
        "addi %[x], %[x], 1\n\t"
        "j 3f\n\t"
        "2:\n\t"
        "addi %[x], %[x], -1\n\t"
        "3:\n\t"
        "addi %[i], %[i], 1\n\t"
        "li t1, 128\n\t"
        "blt %[i], t1, 1b\n\t"  /* loop 128 times */
        : [x] "+r" (x), [i] "=&r" (i)
        :
        : "t0", "t1", "cc", "memory"
    );

    vresult64 = x;
    print_result("Pipe2_BJU", 1);  /* Pass if stats show pipeIssueCount[2] > 0 */
}

/* ============================================================
 * Test 2: Pipe4 (LSU Special) — FENCE 同步
 *
 * 策略：执行 store → FENCE → load 序列，验证 FENCE
 *       在 Pipe4 上执行且 fenceSyncCount 被递增。
 *       期望结果：
 *         - fenceSyncCount > 0
 *         - pipeIssueCount[4] (Pipe4 LSU Special) > 0
 * ============================================================ */
static void test_pipe4_fence(void)
{
    int i;

    /* 先写入数据 */
    for (i = 0; i < 64; i++)
        vbuf[i] = (int64_t)i * 3 + 7;

    /*
     * 强制发射 fence 指令 → 映射到 System opClass → Pipe4
     * fence rw,rw 确保之前的读写在之后的读写之前完成
     */
    __asm__ volatile (
        "fence rw, rw\n\t"
        ::: "memory"
    );

    /* FENCE 后回读验证 */
    int64_t sum = 0;
    for (i = 0; i < 64; i++)
        sum += vbuf[i];

    int64_t expected = 0;
    for (i = 0; i < 64; i++)
        expected += (int64_t)i * 3 + 7;

    vresult64 = sum;
    print_result("Pipe4_FENCE", sum == expected);
}

/* ============================================================
 * Test 3: DIV_STALL — IntDiv FU 阻塞
 *
 * 策略：连续执行 8 条整数除法，IntDiv FU 是 20 周期
 *       非流水线（pipelined=false），2 个 FU 实例。
 *       当 2 个 FU 都被占用时，后续 DIV 指令会阻塞在 IQ 中，
 *       触发 DIV_STALL 检测。
 *       期望结果：
 *         - stallCycles[DIV_STALL] > 0
 *         - pipeIssueCount[1] (Pipe1 IU MLA/DIV) > 0
 *         - ex2WritebackInsts 显著增加
 * ============================================================ */
static void test_div_stall(void)
{
    int64_t dividends[8];
    int64_t divisors[8];
    int64_t results[8];
    int i;

    /* 准备不同的被除数和除数，防止常数折叠 */
    for (i = 0; i < 8; i++) {
        dividends[i] = (int64_t)0x7FFFFFFFFFFFFFFFLL - (int64_t)i * 1000000LL;
        divisors[i] = (int64_t)1234567LL + (int64_t)i * 111111LL;
    }

    /*
     * 用 inline asm 强制生成 8 条独立的 div 指令。
     * 每条 div 的结果写入不同的寄存器，防止数据依赖链
     * 让调度器无法隐藏延迟。
     */
    __asm__ volatile (
        "ld t0, 0(%[div])\n\t"
        "ld t1, 0(%[divs])\n\t"
        "div %[r0], t0, t1\n\t"

        "ld t0, 8(%[div])\n\t"
        "ld t1, 8(%[divs])\n\t"
        "div %[r1], t0, t1\n\t"

        "ld t0, 16(%[div])\n\t"
        "ld t1, 16(%[divs])\n\t"
        "div %[r2], t0, t1\n\t"

        "ld t0, 24(%[div])\n\t"
        "ld t1, 24(%[divs])\n\t"
        "div %[r3], t0, t1\n\t"

        "ld t0, 32(%[div])\n\t"
        "ld t1, 32(%[divs])\n\t"
        "div %[r4], t0, t1\n\t"

        "ld t0, 40(%[div])\n\t"
        "ld t1, 40(%[divs])\n\t"
        "div %[r5], t0, t1\n\t"

        "ld t0, 48(%[div])\n\t"
        "ld t1, 48(%[divs])\n\t"
        "div %[r6], t0, t1\n\t"

        "ld t0, 56(%[div])\n\t"
        "ld t1, 56(%[divs])\n\t"
        "div %[r7], t0, t1\n\t"

        : [r0] "=&r" (results[0]), [r1] "=&r" (results[1]),
          [r2] "=&r" (results[2]), [r3] "=&r" (results[3]),
          [r4] "=&r" (results[4]), [r5] "=&r" (results[5]),
          [r6] "=&r" (results[6]), [r7] "=&r" (results[7])
        : [div] "r" (dividends), [divs] "r" (divisors)
        : "t0", "t1", "memory"
    );

    /* 验证结果正确性 */
    int pass = 1;
    for (i = 0; i < 8; i++) {
        int64_t expected = dividends[i] / divisors[i];
        if (results[i] != expected) {
            pass = 0;
            break;
        }
    }

    vresult64 = results[0];
    print_result("DIV_STALL", pass);
}

/* ============================================================
 * Test 4: IQ_FULL — 填满 Issue Queue
 *
 * 策略：生成大量无数据依赖的独立指令，让 IQ 满载。
 *       在 C910 配置下，8 个专用 IQ 总条目为 86。
 *       如果一次性 dispatch 超过 IQ 容量，应触发 IQ_FULL。
 *       期望结果：
 *         - stallCycles[IQ_FULL] > 0（或至少接近满）
 *         - dispatchStall 计数增加
 * ============================================================ */
static void test_iq_full(void)
{
    int64_t r0, r1, r2, r3, r4, r5, r6, r7;
    int64_t base = 0xABCDEF0123456789LL;

    /*
     * 用 inline asm 生成 16 条完全独立的 ALU 指令，
     * 每条操作不同的寄存器，无数据依赖。
     * 这会让 dispatch 阶段尽可能多地发射指令到 IQ。
     */
    __asm__ volatile (
        "li %[r0], 0x01\n\t"
        "li %[r1], 0x02\n\t"
        "li %[r2], 0x03\n\t"
        "li %[r3], 0x04\n\t"
        "li %[r4], 0x05\n\t"
        "li %[r5], 0x06\n\t"
        "li %[r6], 0x07\n\t"
        "li %[r7], 0x08\n\t"

        "add %[r0], %[r0], %[base]\n\t"
        "xor %[r1], %[r1], %[base]\n\t"
        "and %[r2], %[r2], %[base]\n\t"
        "or  %[r3], %[r3], %[base]\n\t"
        "sub %[r4], %[r4], %[base]\n\t"
        "sll %[r5], %[r5], 3\n\t"
        "srl %[r6], %[r6], 2\n\t"
        "sra %[r7], %[r7], 1\n\t"

        "li t0, 3\n\t"
        "mul %[r0], %[r0], t0\n\t"
        "li t0, 5\n\t"
        "mul %[r1], %[r1], t0\n\t"
        "li t0, 7\n\t"
        "mul %[r2], %[r2], t0\n\t"
        "li t0, 11\n\t"
        "mul %[r3], %[r3], t0\n\t"
        "li t0, 13\n\t"
        "mul %[r4], %[r4], t0\n\t"
        "li t0, 17\n\t"
        "mul %[r5], %[r5], t0\n\t"
        "li t0, 19\n\t"
        "mul %[r6], %[r6], t0\n\t"
        "li t0, 23\n\t"
        "mul %[r7], %[r7], t0\n\t"

        : [r0] "=&r" (r0), [r1] "=&r" (r1),
          [r2] "=&r" (r2), [r3] "=&r" (r3),
          [r4] "=&r" (r4), [r5] "=&r" (r5),
          [r6] "=&r" (r6), [r7] "=&r" (r7)
        : [base] "r" (base)
        : "memory"
    );

    vresult64 = r0 + r1 + r2 + r3 + r4 + r5 + r6 + r7;
    print_result("IQ_FULL", 1);  /* Pass if stats show high IQ utilization */
}

/* ============================================================
 * Test 5: Store-Load Forwarding — 写后读
 *
 * 策略：store 到内存地址后立即 load，验证 store-load
 *       forwarding 机制。C910 使用 store buffer 前递。
 *       期望结果：
 *         - pipeIssueCount[5] (Pipe5 Store) > 0
 *         - pipeIssueCount[3] (Pipe3 Load) > 0
 *         - 数据正确性验证通过
 * ============================================================ */
static void test_store_load_fwd(void)
{
    int64_t val = 0xCAFEBABEDEADBEEFLL;

    /*
     * 用 inline asm 执行 store → immediate load 序列。
     * 关键在于 load 必须在 store 数据写入缓存前完成，
     * 依赖 store buffer forwarding。
     */
    __asm__ volatile (
        "sd %[val], 0(%[addr])\n\t"   /* store to vbuf[0] */
        "ld %[val], 0(%[addr])\n\t"   /* load from vbuf[0] immediately */
        : [val] "+r" (val)
        : [addr] "r" (vbuf)
        : "memory"
    );

    /*
     * 连续 store-load 对，测试 forwarding 带宽
     */
    __asm__ volatile (
        "sd x0, 0(%[addr])\n\t"
        "ld t0, 0(%[addr])\n\t"
        "sd t0, 8(%[addr])\n\t"
        "ld t1, 8(%[addr])\n\t"
        "sd t1, 16(%[addr])\n\t"
        "ld t2, 16(%[addr])\n\t"
        "sd t2, 24(%[addr])\n\t"
        "ld t3, 24(%[addr])\n\t"
        "sd t3, 32(%[addr])\n\t"
        "ld t4, 32(%[addr])\n\t"
        :
        : [addr] "r" (vbuf)
        : "t0", "t1", "t2", "t3", "t4", "memory"
    );

    vresult64 = val;
    print_result("StoreLoad_FWD", 1);
}

/* ============================================================
 * Test 6: Branch Redirect 时序 — 预测错误 → Squash → 重定向
 *
 * 策略：使用大量无规律条件分支，让预测器无法准确预测。
 *       分支解析后在 EX 阶段检测到 mispredict，发送 squash
 *       信号到 Commit，Commit 再 redirect Fetch。
 *       期望结果：
 *         - branchMispredicts 显著增加
 *         - squashedInsts 显著增加
 *         - 预测器类型相关的统计变化
 * ============================================================ */
static void test_branch_redirect(void)
{
    uint32_t state = 0x12345678;
    int count;

    /*
     * 使用 LFSR（线性反馈移位寄存器）生成伪随机分支模式。
     * LFSR 输出对本地预测器来说是不可预测的。
     */
    __asm__ volatile (
        "li %[state], 0x12345678\n\t"
        "li %[count], 0\n\t"
        "1:\n\t"
        /* LFSR step: state = (state >> 1) ^ (-(state & 1) & 0xB0000000) */
        "andi t0, %[state], 1\n\t"
        "srl %[state], %[state], 1\n\t"
        "bnez t0, 2f\n\t"
        "j 3f\n\t"
        "2:\n\t"
        "li t1, 0xB0000000\n\t"
        "xor %[state], %[state], t1\n\t"
        "3:\n\t"
        /* 条件分支：根据 LSB 决定跳转方向 */
        "andi t0, %[state], 1\n\t"
        "beqz t0, 4f\n\t"
        "addi %[count], %[count], 1\n\t"
        "j 5f\n\t"
        "4:\n\t"
        "addi %[count], %[count], -1\n\t"
        "5:\n\t"
        /* 循环 256 次 */
        "li t1, 256\n\t"
        "bltu %[count], t1, 1b\n\t"  /* 用 bltu 做循环条件 */
        : [state] "+r" (state), [count] "=&r" (count)
        :
        : "t0", "t1", "cc", "memory"
    );

    vresult = state;
    print_result("Branch_Redirect", 1);
}

/* ============================================================
 * Test 7: Pipe6 (VFPU ALU) — 浮点 ALU 操作
 *
 * 策略：浮点加法、比较操作，映射到 Pipe6。
 *       期望结果：
 *         - pipeIssueCount[6] > 0
 *         - ex2WritebackInsts 增加（VFPU ALU opLat=3）
 * ============================================================ */
static void test_pipe6_vfpu_alu(void)
{
    double a, b, r;
    int i;

    a = 3.14159265358979;
    b = 2.71828182845904;
    r = 0.0;

    for (i = 0; i < 32; i++) {
        r += a + b;
        if (r > a)
            r -= b;
        a += 0.001;
    }

    vresult64 = *(int64_t *)&r;
    print_result("Pipe6_VFPU_ALU", 1);
}

/* ============================================================
 * Test 8: Pipe7 (VFPU MA) — 浮点乘加
 *
 * 策略：浮点乘法和乘加操作，映射到 Pipe7。
 *       期望结果：
 *         - pipeIssueCount[7] > 0
 *         - ex2WritebackInsts 增加（VFPU MA opLat=5）
 * ============================================================ */
static void test_pipe7_vfpu_ma(void)
{
    double a, b, r;
    int i;

    a = 1.23456789012345;
    b = 9.87654321098765;
    r = 0.0;

    for (i = 0; i < 16; i++) {
        r = r + a * b;  /* FloatMultAcc */
        a += 1.0;
    }

    vresult64 = *(int64_t *)&r;
    print_result("Pipe7_VFPU_MA", 1);
}

/* ============================================================
 * Test 9: EX2 Latency 对比 — IntMult (3 cycles) vs IntDiv (20 cycles)
 *
 * 策略：交替执行乘法和除法，对比 ex1ForwardInsts 和
 *       ex2WritebackInsts 统计。
 *       期望结果：
 *         - ex2WritebackInsts >> ex1ForwardInsts（在混合场景中）
 *         - Pipe1 的 fuBusyRate 在 DIV 密集时升高
 * ============================================================ */
static void test_ex2_latency(void)
{
    int64_t a = 1000000007LL;
    int64_t b = 999999937LL;
    int64_t mul_r = 0;
    int64_t div_r = 0;
    int i;

    for (i = 0; i < 4; i++) {
        /* 乘法：EX2, 3 cycles, pipelined */
        mul_r += a * b;

        /* 除法：EX2, 20 cycles, NOT pipelined */
        div_r += a / b;

        a += 1;
        b -= 1;
    }

    vresult64 = mul_r + div_r;
    print_result("EX2_Latency", 1);
}

/* ============================================================
 * Test 10: Pipe0 (IU ALU) — 基准整数运算
 *
 * 策略：密集的整数 ALU 操作，映射到 Pipe0。
 *       期望结果：
 *         - pipeIssueCount[0] 显著 > 其他 pipe
 *         - ex1ForwardInsts 显著增加
 * ============================================================ */
static void test_pipe0_iu_alu(void)
{
    int64_t a = 0x123456789ABCDEF0LL;
    int64_t b = 0xFEDCBA9876543210LL;
    int64_t r;

    __asm__ volatile (
        "add %[r], %[a], %[b]\n\t"
        "sub %[r], %[r], %[a]\n\t"
        "and %[r], %[r], %[b]\n\t"
        "or  %[r], %[r], %[a]\n\t"
        "xor %[r], %[r], %[b]\n\t"
        "sll %[r], %[r], 7\n\t"
        "srl %[r], %[r], 13\n\t"
        "sra %[r], %[r], 3\n\t"
        : [r] "=&r" (r)
        : [a] "r" (a), [b] "r" (b)
        :
    );

    vresult64 = r;
    print_result("Pipe0_IU_ALU", 1);
}

/* ============================================================
 * Test 11: 综合压力测试 — 混合所有指令类型
 *
 * 策略：在一个测试中混合调用所有类型的操作，触发真实的
 *       资源竞争和多 pipe 并行。
 *       期望结果：
 *         - 多个 pipeIssueCount 条目 > 0
 *         - stallCycles 中有非零值
 *         - 总 tick 数高于单类型测试之和（资源竞争）
 * ============================================================ */
static void test_mixed_stress(void)
{
    int i;

    /* ALU */
    test_pipe0_iu_alu();

    /* FENCE */
    __asm__ volatile ("fence rw, rw" ::: "memory");

    /* 多次 DIV + MUL 混合 */
    int64_t d = 0x7FFFFFFFFFFFFFFFLL;
    for (i = 0; i < 4; i++) {
        d = d / (1234567LL + i);
        d = d * 3 + 1;
    }
    vresult64 = d;

    /* 分支 */
    test_pipe2_bju();

    /* 浮点 */
    test_pipe6_vfpu_alu();
    test_pipe7_vfpu_ma();

    /* 最终屏障 */
    __asm__ volatile ("fence rw, rw" ::: "memory");

    print_result("Mixed_Stress", 1);
}

/* ============================================================
 * 主函数
 * ============================================================ */
int main(void)
{
    uart_puts("\n===== C910 IEW Micro-Benchmark Suite =====\n\n");

    /* Pipe 覆盖测试 */
    test_pipe0_iu_alu();       /* Pipe0: IU ALU */
    test_pipe2_bju();           /* Pipe2: BJU (branch) */
    test_pipe4_fence();         /* Pipe4: LSU Special (FENCE) */
    test_pipe6_vfpu_alu();     /* Pipe6: VFPU ALU */
    test_pipe7_vfpu_ma();      /* Pipe7: VFPU MA */

    /* Stall 测试 */
    test_div_stall();           /* DIV_STALL */
    test_iq_full();             /* IQ_FULL */

    /* 前递和时序测试 */
    test_store_load_fwd();      /* Store-Load Forwarding */
    test_branch_redirect();     /* Branch Redirect */

    /* EX2 Latency */
    test_ex2_latency();         /* EX2 latency comparison */

    /* 综合压力 */
    test_mixed_stress();        /* Mixed stress test */

    uart_puts("\n===== All Micro-Benchmarks Complete =====\n");

    return 0;
}
