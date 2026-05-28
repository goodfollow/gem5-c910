/*
 * c910_comprehensive.c — 综合测试程序
 *
 * 目标：用 printf 可见输出覆盖所有 C910 IEW 功能点
 * - 8 Pipe 全部触发
 * - EX1/EX2 路径
 * - Stall 场景（ROB/IQ 满）
 * - Store-Load 转发
 * - Branch mispredict
 * - FENCE 同步
 *
 * 编译：riscv64-linux-gnu-gcc -O2 -static -o c910_comprehensive c910_comprehensive.c
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

static volatile uint64_t g_sink;

/* ============================================================
 * Pipe0 — IU ALU (ADD/SUB/AND/OR/XOR/Shift, EX1, 1 cycle)
 * ============================================================ */
static uint64_t test_pipe0_alu(void)
{
    uint64_t x = 0xDEADBEEF;
    uint64_t r = 0;
    int i;

    printf("[Pipe0] IU ALU: ");
    for (i = 0; i < 100000; i++) {
        r = (r ^ x) + (x >> 3) - (x << 1) + (x & r) | (x ^ r);
        r = (r >> 7) ^ (r << 5) + 0x5555;
    }
    printf("result=0x%lx\n", (unsigned long)r);
    return r;
}

/* ============================================================
 * Pipe1 — IU MLA/DIV (MUL/DIV, EX2 multi-cycle)
 * ============================================================ */
static void test_pipe1_mla_div(void)
{
    uint64_t a = 123456789ULL;
    uint64_t b = 987654321ULL;
    uint64_t mul_result, div_result;
    int i;

    /* MUL — EX2, multi-cycle */
    mul_result = 0;
    for (i = 0; i < 10000; i++) {
        mul_result += a * b;
    }
    printf("[Pipe1] MUL(10000x): result=0x%lx\n", (unsigned long)mul_result);

    /* DIV — EX2, ~20 cycles per op, triggers DIV_STALL */
    div_result = 0;
    for (i = 0; i < 500; i++) {
        div_result += (a + i) / (b / 1000000 + 1);
    }
    printf("[Pipe1] DIV(500x): result=0x%lx (triggers DIV_STALL)\n",
           (unsigned long)div_result);
}

/* ============================================================
 * Pipe2 — BJU (Branch/Jump Unit, mispredict detection)
 * ============================================================ */
static void test_pipe2_bju(void)
{
    /* 用伪随机方式产生大量分支，触发 mispredict */
    int count = 0;
    uint64_t x = 0x12345678;
    int i;

    printf("[Pipe2] BJU: ");
    for (i = 0; i < 100000; i++) {
        x = x * 1103515245 + 12345;  /* LFSR */
        if ((x & 0xFFFF) > 0x8000)   /* 50% taken — hard to predict */
            count++;
    }
    printf("taken=%d/100000\n", count);
}

/* ============================================================
 * Pipe3 — LSU Load (load data from memory)
 * ============================================================ */
static uint64_t test_pipe3_load(void)
{
    static uint64_t buf[4096];
    uint64_t sum = 0;
    int i;

    /* 先填充 */
    for (i = 0; i < 4096; i++)
        buf[i] = i * 7 + 3;

    /* 顺序 load */
    printf("[Pipe3] LSU Load(4096x): ");
    for (i = 0; i < 4096; i++)
        sum += buf[i];
    printf("sum=%lu\n", (unsigned long)sum);

    /* 随机 load — 更多 cache miss 延迟 */
    sum = 0;
    uint64_t idx = 1;
    for (i = 0; i < 4096; i++) {
        idx = (idx * 1103515245 + 12345) & 0xFFF;
        sum += buf[idx];
    }
    printf("[Pipe3] LSU Load(random 4096x): sum=%lu\n", (unsigned long)sum);
    return sum;
}

/* ============================================================
 * Pipe4 — LSU Special (FENCE / atomic operations)
 * ============================================================ */
static void test_pipe4_fence(void)
{
    int64_t x = 42;
    int i;

    printf("[Pipe4] LSU Special: ");
    for (i = 0; i < 25; i++) {
        __asm__ volatile ("fence rw, rw" ::: "memory");
        x = x * 3 + 7;
    }
    printf("after 25 FENCEs: x=%ld\n", (long)x);
}

/* ============================================================
 * Pipe5 — LSU Store (store data to memory)
 * ============================================================ */
static void test_pipe5_store(void)
{
    static uint64_t buf[2048];
    int i;

    printf("[Pipe5] LSU Store(2048x): ");
    for (i = 0; i < 2048; i++)
        buf[i] = (uint64_t)i * 13 + 7;

    /* 验证 */
    uint64_t sum = 0;
    for (i = 0; i < 2048; i++)
        sum += buf[i];
    printf("stored 2048 values, checksum=%lu\n", (unsigned long)sum);
}

/* ============================================================
 * Store-Load Forwarding — store buffer 转发到 load
 * ============================================================ */
static void test_store_load_forwarding(void)
{
    static uint64_t buf[256];
    int i, errors = 0;

    printf("[SLF] Store-Load Forwarding: ");
    for (i = 0; i < 256; i++) {
        buf[i] = (uint64_t)i * 0xAAAAAAAA + 0x55555555;
        /* 立即 load 刚 store 的值 — 必须从 store buffer 转发 */
        if (buf[i] != (uint64_t)i * 0xAAAAAAAA + 0x55555555)
            errors++;
    }
    printf("errors=%d (expect 0)\n", errors);
}

/* ============================================================
 * Pipe6 — VFPU ALU (FloatAdd/FloatCmp, EX2, 3 cycles)
 * ============================================================ */
static void test_pipe6_vfpu_alu(void)
{
    double a = 3.14159265358979;
    double b = 2.71828182845904;
    double r = 0.0;
    int i;

    printf("[Pipe6] VFPU ALU(1000x): ");
    for (i = 0; i < 1000; i++) {
        r += a + b;       /* FloatAdd, EX2, 3c */
        if (r > a * 100)  /* FloatCmp */
            r -= b;
        a += 0.001;
    }
    printf("result=%.6f\n", r);
}

/* ============================================================
 * Pipe7 — VFPU MA (FloatMult/FloatMultAcc, EX2, 5 cycles)
 * ============================================================ */
static void test_pipe7_vfpu_ma(void)
{
    double a = 1.23456789012345;
    double b = 9.87654321098765;
    double r = 0.0;
    int i;

    printf("[Pipe7] VFPU MA(500x): ");
    for (i = 0; i < 500; i++) {
        r = r + a * b;    /* FloatMultAcc, EX2, 5c */
        a += 0.1;
    }
    printf("result=%.6f\n", r);
}

/* ============================================================
 * EX2 延迟直方图 — 混合不同延迟的指令
 * ============================================================ */
static void test_ex2_histogram(void)
{
    uint64_t a = 12345, b = 67890, mul_r = 0;
    double fa = 1.5, fb = 2.5, fr = 0.0;
    int i;

    printf("[EX2 Hist] Mixed latency:\n");

    /* MUL — EX2 3c */
    for (i = 0; i < 100; i++)
        mul_r += a * b;
    printf("  MUL(100x):   0x%lx (EX2 3c)\n", (unsigned long)mul_r);

    /* FP MUL — EX2 5c */
    fr = 0.0;
    for (i = 0; i < 100; i++)
        fr += fa * fb;
    printf("  FP_MUL(100x): %.6f (EX2 5c)\n", fr);

    /* INT DIV — EX2 20c, triggers DIV_STALL */
    mul_r = 0;
    for (i = 0; i < 50; i++)
        mul_r += (a * 1000 + i) / (b / 1000 + 1);
    printf("  DIV(50x):    0x%lx (EX2 20c, DIV_STALL)\n", (unsigned long)mul_r);
}

/* ============================================================
 * 综合压力 — 混合所有操作触发 Stall
 * ============================================================ */
static void test_stress(void)
{
    uint64_t x = 1, sum = 0;
    double f = 1.0, fsum = 0.0;
    int i;

    printf("[STRESS] Mixed ALU+MUL+DIV+Branch: ");
    for (i = 0; i < 50000; i++) {
        /* ALU + MUL */
        x = x * 31 + 17;
        x ^= x >> 13;
        x &= 0xFFFFFFFFULL;

        /* 分支 */
        if (x > 0x80000000)
            sum += x;

        /* DIV */
        if (i % 100 == 0)
            sum += x / 37;
    }

    /* FP */
    for (i = 0; i < 5000; i++) {
        fsum += f * 1.001;
        if (fsum > 10000.0)
            fsum -= 5000.0;
    }

    printf("int_sum=0x%lx, fp_sum=%.2f\n", (unsigned long)sum, fsum);
}

/* ============================================================
 * 主函数
 * ============================================================ */
int main(void)
{
    printf("========================================\n");
    printf(" C910 IEW Comprehensive Test\n");
    printf("========================================\n\n");

    /* Pipe0: IU ALU */
    g_sink = test_pipe0_alu();
    printf("\n");

    /* Pipe1: MUL + DIV (EX2 multi-cycle, DIV_STALL) */
    test_pipe1_mla_div();
    printf("\n");

    /* Pipe2: BJU branch mispredict */
    test_pipe2_bju();
    printf("\n");

    /* Pipe3: LSU Load */
    g_sink = test_pipe3_load();
    printf("\n");

    /* Pipe4: FENCE */
    test_pipe4_fence();
    printf("\n");

    /* Pipe5: LSU Store */
    test_pipe5_store();
    printf("\n");

    /* Store-Load Forwarding */
    test_store_load_forwarding();
    printf("\n");

    /* Pipe6: VFPU ALU */
    test_pipe6_vfpu_alu();
    printf("\n");

    /* Pipe7: VFPU MA */
    test_pipe7_vfpu_ma();
    printf("\n");

    /* EX2 Latency Histogram */
    test_ex2_histogram();
    printf("\n");

    /* Stress: mixed operations */
    test_stress();
    printf("\n");

    /* 最终屏障 */
    __asm__ volatile ("fence rw, rw" ::: "memory");

    printf("========================================\n");
    printf(" All tests completed.\n");
    printf("========================================\n");
    return 0;
}
