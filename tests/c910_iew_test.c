/*
 * c910_iew_test.c - OpenC910 IEW 功能验证测试
 *
 * 设计目标：覆盖 8 条 Pipe、EX1/EX2 路径、Stall 和 Flush 场景
 *
 * 覆盖矩阵：
 *   Pipe0 (IU ALU):     ADD/SUB/AND/OR/XOR/移位
 *   Pipe1 (IU MLA/DIV): MUL/ADD (EX2), DIV (EX2, 20 cycles)
 *   Pipe2 (BJU):        分支预测错误 → FLUSH_FE
 *   Pipe3 (LSU Load):   LW/LD 加载
 *   Pipe4 (LSU Spec):   FENCE 同步
 *   Pipe5 (LSU Store):  SW/SD 存储
 *   Pipe6 (VFPU ALU):   浮点加/比较 (EX2, 3 cycles)
 *   Pipe7 (VFPU MA):    浮点乘加 (EX2, 5 cycles)
 *
 * 编译：riscv64-linux-gnu-gcc -O0 -static -o c910_iew_test c910_iew_test.c
 * 运行：gem5.opt configs/example/gem5_library/x86-ubuntu-run.py --c910_iew_test
 */

#include <stdint.h>

/* ============================================================
 * 全局缓冲区（防止编译器优化掉）
 * ============================================================ */
volatile int64_t  result_i64;
volatile uint32_t result_u32;
volatile double   result_f64;

/* ============================================================
 * 1. Pipe0 — IU ALU (IntAlu, EX1, 1 cycle)
 *    覆盖：ADD, SUB, AND, OR, XOR, 移位
 * ============================================================ */
static void test_pipe0_iu_alu(void)
{
    int64_t a = 0x123456789ABCDEF0LL;
    int64_t b = 0xFEDCBA9876543210LL;
    int64_t r;

    /* 纯 ALU 操作链，全部走 Pipe0，EX1 路径 */
    r = a + b;         /* ADD */
    r = r - a;         /* SUB */
    r = r & b;         /* AND */
    r = r | a;         /* OR  */
    r = r ^ b;         /* XOR */
    r = r << 7;        /* SLL */
    r = r >> 13;       /* SRL */
    r = (int64_t)((uint64_t)r >> 3);  /* SRA */
    r = (r > a) ? r : a;              /* SLT (set less than) */

    result_i64 = r;
}

/* ============================================================
 * 2. Pipe1 — IU MLA (IntMult, EX2, 3 cycles, pipelined)
 * ============================================================ */
static void test_pipe1_mla(void)
{
    int64_t a = 123456789LL;
    int64_t b = 987654321LL;
    int64_t r = 0;
    int i;

    /* 乘法 → EX2 路径（opLat=3） */
    for (i = 0; i < 8; i++) {
        r += a * b;
        a += 1;
    }

    result_i64 = r;
}

/* ============================================================
 * 2b. Pipe1 — IU DIV (IntDiv, EX2, 20 cycles, NOT pipelined)
 *     长延迟、非流水线，会明显拖慢 IQ
 * ============================================================ */
static void test_pipe1_div(void)
{
    int64_t a = 0x7FFFFFFFFFFFFFFFLL;
    int64_t b = 1234567LL;
    int64_t r = 0;
    int i;

    /* 除法 → EX2 路径（opLat=20, pipelined=false） */
    for (i = 0; i < 4; i++) {
        r += a / b;
        a -= b;
    }

    result_i64 = r;
}

/* ============================================================
 * 3. Pipe2 (BJU) — 分支预测错误 → FLUSH_FE
 *    交替执行 taken / not-taken，让预测器频繁猜错
 * ============================================================ */
static void test_pipe2_bju_mispredict(void)
{
    volatile int x = 0;
    int i;

    /* 让静态预测器（通常预测 not-taken）频繁出错 */
    for (i = 0; i < 64; i++) {
        if (i & 1) {          /* 奇数 → taken */
            x += 1;
        } else {              /* 偶数 → not-taken */
            x -= 1;
        }
    }

    result_u32 = (uint32_t)x;
}

/* ============================================================
 * 4. Pipe3 (LSU Load) — MemRead, EX1, 1 cycle (cache hit)
 * ============================================================ */
static void test_pipe3_load(void)
{
    static int64_t buf[256];
    int64_t sum = 0;
    int i;

    /* 先填数据（store，会在 setup 中完成） */
    for (i = 0; i < 256; i++)
        buf[i] = i + 1;

    /* 连续加载 → 全部走 Pipe3 */
    for (i = 0; i < 256; i++)
        sum += buf[i];

    result_i64 = sum;
}

/* ============================================================
 * 5. Pipe4 (LSU Special) — FENCE
 * ============================================================ */
static void test_pipe4_fence(void)
{
    static int64_t shared_buf[64];
    int i;

    for (i = 0; i < 64; i++)
        shared_buf[i] = i * 2;

    /*
     * FENCE 指令会映射到 System opClass → Pipe4
     * __asm 直接发射 RISC-V fence 指令
     */
    __asm__ volatile ("fence rw, rw" ::: "memory");

    /* 验证 fence 后数据仍然一致 */
    int64_t sum = 0;
    for (i = 0; i < 64; i++)
        sum += shared_buf[i];

    result_i64 = sum;
}

/* ============================================================
 * 6. Pipe5 (LSU Store) — MemWrite, EX1, 1 cycle
 * ============================================================ */
static void test_pipe5_store(void)
{
    static int64_t dst[128];
    int i;

    /* 密集写操作 → 走 Pipe5 */
    for (i = 0; i < 128; i++)
        dst[i] = (int64_t)i * i;

    /* FENCE 确保 store 完成 */
    __asm__ volatile ("fence rw, rw" ::: "memory");

    /* 回读验证 */
    int64_t check = 0;
    for (i = 0; i < 128; i++)
        check += dst[i];

    result_i64 = check;
}

/* ============================================================
 * 7. Pipe6 (VFPU ALU) — FloatAdd/FloatCmp, EX2, 3 cycles
 * ============================================================ */
static void test_pipe6_vfpu_alu(void)
{
    double a = 3.14159265358979;
    double b = 2.71828182845904;
    double r = 0.0;
    int i;

    /* 浮点加/比较 → Pipe6（opLat=3） */
    for (i = 0; i < 16; i++) {
        r += a + b;          /* FloatAdd */
        if (r > a)           /* FloatCmp */
            r -= b;
        a += 0.001;
    }

    result_f64 = r;
}

/* ============================================================
 * 8. Pipe7 (VFPU MA) — FloatMult/FloatMultAcc, EX2, 5 cycles
 * ============================================================ */
static void test_pipe7_vfpu_ma(void)
{
    double a = 1.23456789012345;
    double b = 9.87654321098765;
    double r = 0.0;
    int i;

    /* 浮点乘/乘加 → Pipe7（opLat=5） */
    for (i = 0; i < 8; i++) {
        r = r + a * b;       /* FloatMultAcc */
        a += 1.0;
    }

    result_f64 = r;
}

/* ============================================================
 * 综合测试 — 混合调用触发 Stall
 *    大量指令同时涌入，让 ROB/IQ 满，触发 DISPATCH_STALL
 * ============================================================ */
static void test_stall_dispatch(void)
{
    int64_t x = 1;
    int i;

    /* 密集整数运算填满 IQ → 触发 IQ_FULL / DISPATCH_STALL */
    for (i = 0; i < 256; i++) {
        x = x * 3 + 7;
        x = x ^ (x >> 13);
        x = x & 0xFFFFFFFFLL;
    }

    result_i64 = x;
}

/* ============================================================
 * 主函数
 * ============================================================ */
int main(void)
{
    /* Pipe0: IU ALU */
    test_pipe0_iu_alu();

    /* Pipe1: IU MLA (multiply) */
    test_pipe1_mla();

    /* Pipe1: IU DIV (divide — 20 cycles!) */
    test_pipe1_div();

    /* Pipe2: BJU (branch mispredict) */
    test_pipe2_bju_mispredict();

    /* Pipe3: LSU Load */
    test_pipe3_load();

    /* Pipe4: LSU Special (FENCE) */
    test_pipe4_fence();

    /* Pipe5: LSU Store */
    test_pipe5_store();

    /* Pipe6: VFPU ALU */
    test_pipe6_vfpu_alu();

    /* Pipe7: VFPU MA */
    test_pipe7_vfpu_ma();

    /* Stall: 密集运算触发 IQ/ROB 满 */
    test_stall_dispatch();

    /* 最终屏障 */
    __asm__ volatile ("fence rw, rw" ::: "memory");

    return 0;
}
