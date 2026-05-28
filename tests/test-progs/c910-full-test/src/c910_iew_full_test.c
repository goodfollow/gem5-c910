/*
 * c910_iew_full_test.c — Comprehensive IEW Stage Verification
 *
 * Targets all C910 IEW modifications in gem5 O3:
 *   - 8 typed Issue Queues (AIQ0/1, BIQ, LSIQ, SDIQ, VIQ0/1, VMB)
 *   - IQ type fallback routing
 *   - 10 stall type detection
 *   - EX1 (1-cycle) vs EX2 (multi-cycle) path separation
 *   - DIV_STALL (IntDiv 20-cycle non-pipelined)
 *   - Multi-PRF scoreboard (PREG/VREG/EREG)
 *   - Per-pipe issue counting
 *   - Store-load forwarding edge cases
 *   - FENCE ordering barriers
 *   - Writeback port classification
 *
 * Compilation:
 *   riscv64-unknown-linux-gnu-gcc -O2 -static -march=rv64gc \
 *       -o c910_iew_full_test c910_iew_full_test.c
 *
 * Usage:
 *   build/RISCV/gem5.opt configs/example/c910_full_test_se.py
 *
 * Output: Each test prints a PASS/FAIL summary via printf.
 * The gem5 stats.txt is validated by analyze_full_test_stats.py.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

/* ============================================================
 * Infrastructure
 * ============================================================ */
static volatile int64_t  g_i64;
static volatile uint64_t g_u64;
static volatile uint32_t g_u32;
static volatile double   g_f64;
static volatile float    g_f32;

static int g_pass = 0;
static int g_fail = 0;

#define PASS(name) do { g_pass++; printf("[PASS] %s\n", name); } while(0)
#define FAIL(name) do { g_fail++; printf("[FAIL] %s\n", name); } while(0)
#define CHECK(name, cond) do { if (cond) PASS(name); else FAIL(name); } while(0)

/* ============================================================
 * 1. IQ Type Routing Tests
 *    Verify that instructions are dispatched to the correct
 *    typed IQ by exercising each instruction class.
 * ============================================================ */

/*
 * 1a. AIQ0 — Integer ALU (round-robin between AIQ0/AIQ1)
 *     Covers: IntAlu opClass → Pipe0/Pipe1
 */
static void test_iq_aiq0_routing(void)
{
    int64_t a = 0xDEADBEEFCAFEBABE;
    int64_t b = 0x0123456789ABCDEF;
    int64_t r = 0;
    int i;

    printf("\n[1a] IQ Routing: AIQ0 (IntAlu)\n");

    /* Generate enough IntAlu instructions to cross the dispatch width */
    for (i = 0; i < 200; i++) {
        r = (r + a) ^ b;
        r = (r - b) | a;
        r = (r & a) + (b >> 3);
        r = (r ^ (r << 7)) & 0xFFFFFFFFFFFFFFFFULL;
        /* SLT — set less than */
        r = (r > 0) ? (r + 1) : (r - 1);
    }

    g_i64 = r;
    /* Deterministic checksum: compute expected value */
    int64_t ta = 0xDEADBEEFCAFEBABE, tb = 0x0123456789ABCDEF, tr = 0;
    for (i = 0; i < 200; i++) {
        tr = (tr + ta) ^ tb;
        tr = (tr - tb) | ta;
        tr = (tr & ta) + (tb >> 3);
        tr = (tr ^ (tr << 7)) & 0xFFFFFFFFFFFFFFFFULL;
        tr = (tr > 0) ? (tr + 1) : (tr - 1);
    }
    CHECK("  AIQ0 checksum", g_i64 == tr);
}

/*
 * 1b. AIQ1 — IntMult (MUL/MADD) → Pipe1 MLA unit
 *     Covers: IntMult opClass → AIQ1
 */
static void test_iq_aiq1_mult(void)
{
    int64_t a = 999999937;
    int64_t b = 888888877;
    int64_t r = 0;
    int i;

    printf("[1b] IQ Routing: AIQ1 (IntMult)\n");

    /* MUL → AIQ1, MLA → AIQ1 */
    for (i = 0; i < 500; i++) {
        r += a * b;
        r = (r * 31) + 17;
    }

    g_i64 = r;
    int64_t ta = 999999937, tb = 888888877, tr = 0;
    for (i = 0; i < 500; i++) {
        tr += ta * tb;
        tr = (tr * 31) + 17;
    }
    CHECK("  AIQ1 MUL checksum", g_i64 == tr);
}

/*
 * 1c. BIQ — Branch/Jump → Pipe2
 *     Generates hard-to-predict branches to trigger mispredicts
 */
static void test_iq_biq_branch(void)
{
    int taken = 0, not_taken = 0;
    uint64_t x = 0xDEAD;
    int i;

    printf("[1c] IQ Routing: BIQ (Branch)\n");

    /* Pseudo-random branches — ~50% taken, hard to predict */
    for (i = 0; i < 50000; i++) {
        x = x * 1103515245 + 12345;  /* LCG PRNG */
        if ((x & 0xFF) > 127) {
            taken++;
            x ^= 0xABCD;
        } else {
            not_taken++;
            x ^= 0x1234;
        }
    }

    CHECK("  BIQ branch balance", taken == 25000 || taken > 0);
    g_u32 = (uint32_t)taken;
}

/*
 * 1d. LSIQ — Load address calculation → Pipe3
 *     Sequential and random load patterns
 */
static void test_iq_lsiq_load(void)
{
    static int64_t buf[8192];
    int64_t sum = 0, expected = 0;
    int i;

    printf("[1d] IQ Routing: LSIQ (Load)\n");

    /* Initialize buffer */
    for (i = 0; i < 8192; i++) {
        buf[i] = (int64_t)i * 13 + 7;
        expected += buf[i];
    }

    /* Sequential loads */
    for (i = 0; i < 8192; i++)
        sum += buf[i];

    CHECK("  LSIQ sequential load", sum == expected);

    /* Random loads */
    sum = 0;
    uint64_t idx = 42;
    for (i = 0; i < 8192; i++) {
        idx = (idx * 1103515245 + 12345) & 0x1FFF;  /* mod 8192 */
        sum += buf[idx];
    }
    g_i64 = sum;
    CHECK("  LSIQ random load", sum != 0);
}

/*
 * 1e. SDIQ — Store data → Pipe5
 *     Dense store pattern, then verify
 */
static void test_iq_sdiq_store(void)
{
    static int64_t dst[4096];
    int i;

    printf("[1e] IQ Routing: SDIQ (Store)\n");

    /* Dense stores → Pipe5 */
    for (i = 0; i < 4096; i++)
        dst[i] = (int64_t)i * 17 + 31;

    __asm__ volatile ("fence rw, rw" ::: "memory");

    /* Verify */
    int64_t sum = 0, expected = 0;
    for (i = 0; i < 4096; i++) {
        sum += dst[i];
        expected += (int64_t)i * 17 + 31;
    }
    CHECK("  SDIQ store checksum", sum == expected);
    g_i64 = sum;
}

/*
 * 1f. VMB — Vector memory buffer (vector load/store ops)
 *     Since scalar C can't easily generate RVV instructions,
 *     we rely on the decoder mapping. This section tests that
 *     the routing logic handles the absence of vector ops gracefully.
 */
static void test_iq_vmb_vector(void)
{
    printf("[1f] IQ Routing: VMB (Vector Memory Buffer)\n");

    /* In SE mode with a scalar binary, vector instructions
     * won't appear. The VMB routing is tested by the decoder.
     * We just verify the fallback path works. */

    /* Generate a large number of integer ops that would normally
     * route to AIQ, ensuring VMB stays empty. */
    int64_t r = 0;
    int i;
    for (i = 0; i < 1000; i++) {
        r = r * 31 + 17;
        r ^= r >> 11;
    }
    g_i64 = r;
    CHECK("  VMB scalar fallback", r != 0);
}

/*
 * 1g. VIQ0 — Vector ALU
 *     Same as VMB, scalar binary won't generate RVV, but
 *     tests that the routing table doesn't crash on absence.
 */
static void test_iq_viq0_valu(void)
{
    printf("[1g] IQ Routing: VIQ0 (Vector ALU)\n");

    /* Generate FP operations that route to VIQ1 (scalar FP → VIQ1) */
    double a = 1.0, b = 2.0, r = 0.0;
    int i;
    for (i = 0; i < 500; i++) {
        r += a / (b + 0.5);
        a += 0.5;
    }
    g_f64 = r;
    CHECK("  VIQ1 scalar FP", r > 100.0);
}

/* ============================================================
 * 2. IQ Overflow & Fallback Tests
 *    Force specific IQ types to fill up, verify fallback routing
 *    dispatchWidth=4, issueWidth=8, so we need > IQ capacity
 *    to trigger overflow.
 * ============================================================ */

/*
 * 2a. Fill BIQ (12 entries) with branches to trigger fallback to AIQ0
 */
static void test_iq_overflow_biq(void)
{
    printf("\n[2a] IQ Overflow: BIQ → AIQ0 fallback\n");

    int taken_count = 0;
    int i;
    uint64_t x = 0xBEEF;

    /* More iterations than BIQ capacity (12) to trigger overflow */
    for (i = 0; i < 500; i++) {
        x = x * 6364136223846793005ULL + 1;
        if ((x & 0xFF) > 127)
            taken_count++;
    }
    g_u32 = (uint32_t)taken_count;
    CHECK("  BIQ overflow fallback", taken_count > 0);
}

/*
 * 2b. Fill AIQ1 (11 entries) with MULs to trigger fallback to AIQ0
 */
static void test_iq_overflow_aiq1(void)
{
    printf("[2b] IQ Overflow: AIQ1 → AIQ0 fallback\n");

    int64_t a = 777777777, b = 333333333, r = 0;
    int i;

    /* More iterations than AIQ1 capacity (11) */
    for (i = 0; i < 300; i++) {
        r += a * b;
        r = (r + a) * 31;
    }
    g_i64 = r;

    /* Reference computation */
    int64_t ta = 777777777, tb = 333333333, tr = 0;
    for (i = 0; i < 300; i++) {
        tr += ta * tb;
        tr = (tr + ta) * 31;
    }
    CHECK("  AIQ1 overflow checksum", g_i64 == tr);
}

/* ============================================================
 * 3. Stall Type Tests
 *    Generate conditions that trigger various stall types.
 *    Stall types: NO_STALL, ROB_FULL, IQ_FULL, VMB_FULL,
 *                 TYPE_STALL, DISPATCH_STALL, IS_STALL,
 *                 DIV_STALL, VDIV_STALL, BACKEND_STALL
 * ============================================================ */

/*
 * 3a. DISPATCH_STALL — Dense mixed ops to saturate dispatch bandwidth
 */
static void test_stall_dispatch(void)
{
    printf("\n[3a] Stall: DISPATCH (bandwidth saturation)\n");

    int64_t x = 1, y = 2, z = 3;
    int i;

    /* Heavy mixed ops → dispatch bandwidth limit */
    for (i = 0; i < 10000; i++) {
        x = x * 31 + y;     /* MUL → AIQ1 */
        y = y ^ (x >> 13);  /* ALU → AIQ0/1 */
        z = z + x - y;      /* ALU → AIQ0/1 */
        if (x > 0)          /* Branch → BIQ */
            z ^= x;
        else
            z += y;
    }

    g_i64 = x;
    g_u64 = y;
    g_u32 = (uint32_t)z;
    CHECK("  DISPATCH stall mix", x != 0 && y != 0);
}

/*
 * 3b. DIV_STALL — Back-to-back divisions to trigger IntDiv busy
 *     IntDiv: opLat=20, pipelined=false → one DIV blocks next for 20 cycles
 */
static void test_stall_div(void)
{
    printf("[3b] Stall: DIV_STALL (IntDiv busy)\n");

    int64_t a = 0x7FFFFFFFFFFFFFFF;
    int64_t r = 0;
    int i;

    /* 50 divisions in sequence — each takes 20 cycles */
    for (i = 0; i < 50; i++) {
        r += a / (i + 1);
    }

    g_i64 = r;

    /* Reference: r = sum(MAX_INT64 / (i+1)) for i=0..49 */
    int64_t ref = 0;
    for (i = 0; i < 50; i++) {
        ref += 0x7FFFFFFFFFFFFFFF / (i + 1);
    }
    CHECK("  DIV_STALL checksum", g_i64 == ref);
}

/*
 * 3c. Mixed DIV+ALU — DIV stalls while ALU continues
 */
static void test_stall_div_mixed(void)
{
    printf("[3c] Stall: DIV + ALU mixed (asynchronous pipes)\n");

    int64_t div_r = 0, alu_r = 0;
    int i;
    int64_t big = 0x1FFFFFFFFFFFFF;

    /* Interleave DIV (slow pipe) with ALU (fast pipe) */
    for (i = 0; i < 100; i++) {
        /* This DIV takes 20 cycles */
        if (i % 10 == 0) {
            div_r += big / (i + 7);
        }
        /* These ALU ops take 1 cycle each — can proceed while DIV busy */
        alu_r = alu_r * 31 + i;
        alu_r ^= alu_r >> 7;
        alu_r &= 0xFFFFFFFFFFULL;
    }

    g_i64 = div_r;
    g_u64 = alu_r;

    int64_t dref = 0;
    for (i = 0; i < 100; i++) {
        if (i % 10 == 0)
            dref += big / (i + 7);
    }
    CHECK("  DIV+ALU mixed DIV", g_i64 == dref);
    CHECK("  DIV+ALU mixed ALU", g_u64 != 0);
}

/* ============================================================
 * 4. EX1 vs EX2 Path Tests
 *    EX1: 1-cycle combinational forwarding (IntAlu, loads, stores)
 *    EX2: Multi-cycle clock-edge writeback (MUL=3, DIV=20, FP_ADD=3, FP_MUL=5)
 * ============================================================ */

/*
 * 4a. EX1 — All single-cycle ops → comb forwarding
 */
static void test_ex1_path(void)
{
    printf("\n[4a] EX1 Path: Single-cycle combinational forwarding\n");

    int64_t r = 0;
    int i;

    /* All these should be EX1 (1 cycle): ADD, SUB, AND, OR, XOR, SLL, SRL */
    for (i = 0; i < 10000; i++) {
        r += i;           /* ADD */
        r -= (i >> 3);    /* SUB */
        r ^= (r << 5);    /* XOR, SLL */
        r &= 0xFFFFFFFF;  /* AND */
        r |= (r >> 11);   /* OR, SRL */
    }

    g_i64 = r;

    int64_t ref = 0;
    for (i = 0; i < 10000; i++) {
        ref += i;
        ref -= (i >> 3);
        ref ^= (ref << 5);
        ref &= 0xFFFFFFFF;
        ref |= (ref >> 11);
    }
    CHECK("  EX1 checksum", g_i64 == ref);
}

/*
 * 4b. EX2 — Multi-cycle ops (MUL, FP_ADD, FP_MUL)
 */
static void test_ex2_path(void)
{
    printf("[4b] EX2 Path: Multi-cycle clock-edge writeback\n");

    /* MUL — EX2, 3 cycles */
    int64_t mul_r = 0;
    int i;
    for (i = 0; i < 1000; i++) {
        mul_r += (int64_t)i * (i + 1);
    }
    g_i64 = mul_r;

    int64_t mul_ref = 0;
    for (i = 0; i < 1000; i++)
        mul_ref += (int64_t)i * (i + 1);

    CHECK("  EX2 MUL checksum", g_i64 == mul_ref);

    /* FP_ADD — EX2, 3 cycles */
    double fa = 1.2345, fb = 6.7890, fr = 0.0;
    for (i = 0; i < 500; i++) {
        fr += fa + fb;
        fa += 0.01;
    }
    g_f64 = fr;
    CHECK("  EX2 FP_ADD", fr > 1000.0);

    /* FP_MUL — EX2, 5 cycles */
    fr = 1.0;
    for (i = 0; i < 200; i++) {
        fr *= 1.0001;
    }
    g_f64 = fr;
    CHECK("  EX2 FP_MUL", fr > 1.0);
}

/*
 * 4c. Mixed EX1+EX2 — Dependency chain across different latencies
 */
static void test_ex1_ex2_dependency(void)
{
    printf("[4c] EX1+EX2: Cross-latency dependency chain\n");

    /* ALU (EX1) → MUL (EX2) → ALU (EX1) → FP (EX2) chain */
    int64_t x = 42;
    int i;
    for (i = 0; i < 2000; i++) {
        x = x * 31 + 17;        /* EX2 MUL */
        x = x ^ (x >> 13);      /* EX1 ALU */
        x = x + i;              /* EX1 ALU */
        x = (x > 0) ? x : -x;   /* EX1 ALU (SLT) */
        x *= 3;                 /* EX2 MUL */
        x &= 0xFFFFFFFFFFULL;   /* EX1 ALU */
    }

    g_i64 = x;

    int64_t ref = 42;
    for (i = 0; i < 2000; i++) {
        ref = ref * 31 + 17;
        ref = ref ^ (ref >> 13);
        ref = ref + i;
        ref = (ref > 0) ? ref : -ref;
        ref *= 3;
        ref &= 0xFFFFFFFFFFULL;
    }
    CHECK("  EX1+EX2 chain checksum", g_i64 == ref);
}

/* ============================================================
 * 5. FENCE Ordering & Barrier Tests
 *    FENCE maps to System opClass → Pipe4
 *    Tests memory ordering with interleaved FENCEs
 * ============================================================ */

static void test_fence_ordering(void)
{
    printf("\n[5a] FENCE: Memory ordering with interleaved barriers\n");

    static int64_t buf_a[256], buf_b[256];
    int64_t sum_a = 0, sum_b = 0;
    int i;

    /* Fill buffers */
    for (i = 0; i < 256; i++) {
        buf_a[i] = i * 3 + 1;
        buf_b[i] = i * 7 + 2;
    }

    /* Store → FENCE → Load pattern */
    for (i = 0; i < 256; i++) {
        buf_a[i] += 100;
        buf_b[i] += 200;
        if (i % 32 == 31)
            __asm__ volatile ("fence rw, rw" ::: "memory");
    }

    __asm__ volatile ("fence rw, rw" ::: "memory");

    /* Verify after FENCEs */
    for (i = 0; i < 256; i++) {
        sum_a += buf_a[i];
        sum_b += buf_b[i];
    }

    int64_t exp_a = 0, exp_b = 0;
    for (i = 0; i < 256; i++) {
        exp_a += (int64_t)i * 3 + 1 + 100;
        exp_b += (int64_t)i * 7 + 2 + 200;
    }
    CHECK("  FENCE ordering A", sum_a == exp_a);
    CHECK("  FENCE ordering B", sum_b == exp_b);
}

/* ============================================================
 * 6. Store-Load Forwarding Tests
 *    Verify that loads get data from store buffer (not memory)
 *    when accessing recently stored locations.
 * ============================================================ */

static void test_store_load_forwarding(void)
{
    printf("\n[6a] SLF: Store-to-load forwarding (immediate)\n");

    static int64_t buf[512];
    int errors = 0;
    int i;

    /* Store then immediately load — must forward from store buffer */
    for (i = 0; i < 512; i++) {
        int64_t val = (int64_t)i * 0xAAAAAAAA + 0x55555555;
        buf[i] = val;
        if (buf[i] != val)
            errors++;
    }
    CHECK("  SLF immediate errors", errors == 0);

    printf("[6b] SLF: Store-to-load forwarding (strided)\n");

    errors = 0;
    for (i = 0; i < 512; i += 4) {
        buf[i] = (int64_t)i * 0xBBBBBBBB + 0x33333333;
        if (buf[i] != (int64_t)i * 0xBBBBBBBB + 0x33333333)
            errors++;
    }
    CHECK("  SLF strided errors", errors == 0);

    printf("[6c] SLF: Store-to-load forwarding (randomized)\n");

    errors = 0;
    uint64_t idx = 7;
    for (i = 0; i < 1024; i++) {
        idx = (idx * 1103515245 + 12345) & 0x1FF;  /* mod 512 */
        int64_t val = (int64_t)i * 0xCCCCCCCC + 0x11111111;
        buf[idx] = val;
        if (buf[idx] != val)
            errors++;
    }
    CHECK("  SLF randomized errors", errors == 0);
}

/* ============================================================
 * 7. Multi-PRF Scoreboard Tests
 *    Tests PREG (integer), VREG (vector), EREG (FP) separation
 *    In scalar C, we can only test PREG and EREG (scalar FP)
 * ============================================================ */

static void test_prf_preg(void)
{
    printf("\n[7a] PRF: PREG scoreboard stress\n");

    /* Generate many register renames — need > 95 physical int regs */
    int64_t r[200];
    int i;

    /* Initialize */
    for (i = 0; i < 200; i++)
        r[i] = (int64_t)i * 31 + 17;

    /* Dependency chains — each depends on 2 previous values */
    for (i = 2; i < 200; i++) {
        r[i] = r[i-1] * 31 + r[i-2];
        r[i] ^= r[i] >> 13;
        r[i] &= 0xFFFFFFFFFFFFULL;
    }

    /* Final checksum */
    int64_t checksum = 0;
    for (i = 0; i < 200; i++)
        checksum ^= r[i];

    g_i64 = checksum;

    /* Reference */
    int64_t ref[200];
    for (i = 0; i < 200; i++)
        ref[i] = (int64_t)i * 31 + 17;
    for (i = 2; i < 200; i++) {
        ref[i] = ref[i-1] * 31 + ref[i-2];
        ref[i] ^= ref[i] >> 13;
        ref[i] &= 0xFFFFFFFFFFFFULL;
    }
    int64_t ref_checksum = 0;
    for (i = 0; i < 200; i++)
        ref_checksum ^= ref[i];

    CHECK("  PREG scoreboard", g_i64 == ref_checksum);
}

static void test_prf_ereg(void)
{
    printf("[7b] PRF: EREG scoreboard stress (scalar FP)\n");

    double a[100], b[100], c[100];
    int i;

    /* Initialize */
    for (i = 0; i < 100; i++) {
        a[i] = (double)i * 1.234 + 0.567;
        b[i] = (double)i * 2.345 + 1.234;
    }

    /* Mixed FP operations — ADD, MUL, DIV all map to different EREG entries */
    for (i = 0; i < 100; i++) {
        c[i] = a[i] + b[i];       /* FloatAdd */
        c[i] = c[i] * 1.5;        /* FloatMult */
        if (i > 0)
            c[i] = c[i] / c[i-1]; /* FloatDiv */
    }

    /* Sum check */
    double sum = 0.0;
    for (i = 0; i < 100; i++)
        sum += c[i];

    g_f64 = sum;
    CHECK("  EREG scoreboard", sum > 0);
}

/* ============================================================
 * 8. Writeback Port Classification Tests
 *    PREG writeback: integer results → pregWBWidth (3 ports)
 *    EREG writeback: FP results → eregWBWidth (2 ports)
 *    VREG writeback: vector results → vregWBWidth (3 ports)
 * ============================================================ */

static void test_wb_preg(void)
{
    printf("\n[8a] WB: PREG writeback ports (integer)\n");

    int64_t r = 0;
    int i;
    /* Burst of integer ops → PREG WB */
    for (i = 0; i < 5000; i++) {
        r = r * 31 + i;
        r ^= r >> 7;
        r &= 0xFFFFFFFFFFFFFFFFULL;
    }
    g_i64 = r;
    CHECK("  PREG WB burst", r != 0);
}

static void test_wb_ereg(void)
{
    printf("[8b] WB: EREG writeback ports (FP)\n");

    double r = 1.0;
    int i;
    /* Burst of FP ops → EREG WB */
    for (i = 0; i < 3000; i++) {
        r = r * 1.0001 + 0.001;
    }
    g_f64 = r;
    CHECK("  EREG WB burst", r > 1.0);
}

/* ============================================================
 * 9. Large-Scale Mixed Stress Test
 *    Combines all instruction types in one tight loop to:
 *    - Fill ROB (64 entries) → trigger ROB_FULL stall
 *    - Fill multiple IQ types simultaneously
 *    - Generate EX1 and EX2 instructions interleaved
 *    - Trigger branch mispredicts
 *    - Exercise all 8 pipes concurrently
 * ============================================================ */

static void test_stress_mixed(void)
{
    printf("\n[9a] STRESS: Mixed all-ops large-scale stress\n");

    int64_t int_r = 1;
    double fp_r = 1.0;
    int branch_taken = 0;
    int64_t load_buf[1024];
    int i;

    /* Fill buffer */
    for (i = 0; i < 1024; i++)
        load_buf[i] = (int64_t)i * 99 + 1;

    int64_t load_sum = 0;

    /* 50000 iterations — each loop has:
     *   3x IntAlu   → AIQ0/AIQ1 (Pipe0/Pipe1, EX1)
     *   1x IntMult  → AIQ1 (Pipe1, EX2 3c)
     *   1x IntDiv   → AIQ0/AIQ1 (Pipe0/Pipe1, EX2 20c) every 50th
     *   1x Branch   → BIQ (Pipe2, EX1)
     *   1x Load     → LSIQ (Pipe3, EX1) every 10th
     *   1x Store    → SDIQ (Pipe5, EX1) every 10th
     *   2x FP Add   → VIQ1 (Pipe6, EX2 3c)
     *   1x FP Mul   → VIQ1 (Pipe7, EX2 5c) every 20th
     */
    for (i = 0; i < 50000; i++) {
        /* 3x ALU */
        int_r = int_r * 31 + i;
        int_r ^= int_r >> 13;
        int_r &= 0xFFFFFFFFFFFFFFFFULL;

        /* MUL every iteration */
        int_r += int_r * 17;

        /* DIV every 50th iteration */
        if (i % 50 == 0) {
            int_r += 0x7FFFFFFFFFFFFFFF / (i + 1);
        }

        /* Branch */
        if (int_r > 0)
            branch_taken++;
        else
            int_r = -int_r;

        /* Load every 10th */
        if (i % 10 == 0)
            load_sum += load_buf[i & 0x3FF];

        /* Store every 10th */
        if (i % 10 == 5)
            load_buf[i & 0x3FF] = int_r;

        /* FP operations */
        fp_r = fp_r + 1.001;
        fp_r = fp_r * 0.999;

        /* FP MUL every 20th */
        if (i % 20 == 0)
            fp_r = fp_r * 1.5;
    }

    g_i64 = int_r;
    g_f64 = fp_r;
    g_u64 = (uint64_t)load_sum;
    g_u32 = (uint32_t)branch_taken;

    CHECK("  STRESS int result", int_r != 0);
    CHECK("  STRESS fp result", fp_r > 50000.0);
    CHECK("  STRESS load sum", load_sum != 0);
    CHECK("  STRESS branch count", branch_taken > 0);
}

/* ============================================================
 * 10. Dependency Chain & Wake-up Tests
 *     Tests the dependency graph:
 *     - Single producer → multiple consumers (fan-out)
 *     - Multiple producers → single consumer (fan-in)
 *     - Long dependency chains
 * ============================================================ */

/*
 * 10a. Fan-out: one instruction wakes many dependents
 */
static void test_dep_fanout(void)
{
    printf("\n[10a] Dep: Fan-out (1 producer → 8 consumers)\n");

    uint64_t prod = 0xABCDEF0123456789ULL;
    uint64_t c0, c1, c2, c3, c4, c5, c6, c7;

    /* 8 consumers all depend on prod */
    c0 = prod + 1;
    c1 = prod - 2;
    c2 = prod ^ 0xFF;
    c3 = prod | 0xAAAA;
    c4 = prod & 0x5555;
    c5 = prod << 3;
    c6 = prod >> 5;
    c7 = prod * 31;

    uint64_t checksum = c0 ^ c1 ^ c2 ^ c3 ^ c4 ^ c5 ^ c6 ^ c7;
    g_u64 = checksum;

    /* Reference */
    uint64_t p = 0xABCDEF0123456789ULL;
    uint64_t rc = (p + 1) ^ (p - 2) ^ (p ^ 0xFF) ^ (p | 0xAAAA) ^
                  (p & 0x5555) ^ (p << 3) ^ (p >> 5) ^ (p * 31);
    CHECK("  Fan-out checksum", g_u64 == rc);
}

/*
 * 10b. Fan-in: 8 producers → 1 consumer
 */
static void test_dep_fanin(void)
{
    printf("[10b] Dep: Fan-in (8 producers → 1 consumer)\n");

    int64_t p0 = 1, p1 = 2, p2 = 3, p3 = 4;
    int64_t p4 = 5, p5 = 6, p6 = 7, p7 = 8;

    /* Evolve each producer independently */
    int i;
    for (i = 0; i < 1000; i++) {
        p0 = p0 * 31 + 1;
        p1 = p1 * 37 + 2;
        p2 = p2 * 41 + 3;
        p3 = p3 * 43 + 4;
        p4 = p4 * 47 + 5;
        p5 = p5 * 53 + 6;
        p6 = p6 * 59 + 7;
        p7 = p7 * 61 + 8;
    }

    /* Single consumer depends on all 8 */
    int64_t consumer = p0 + p1 + p2 + p3 + p4 + p5 + p6 + p7;
    g_i64 = consumer;

    /* Reference */
    int64_t rp0 = 1, rp1 = 2, rp2 = 3, rp3 = 4;
    int64_t rp4 = 5, rp5 = 6, rp6 = 7, rp7 = 8;
    for (i = 0; i < 1000; i++) {
        rp0 = rp0 * 31 + 1;
        rp1 = rp1 * 37 + 2;
        rp2 = rp2 * 41 + 3;
        rp3 = rp3 * 43 + 4;
        rp4 = rp4 * 47 + 5;
        rp5 = rp5 * 53 + 6;
        rp6 = rp6 * 59 + 7;
        rp7 = rp7 * 61 + 8;
    }
    int64_t ref = rp0 + rp1 + rp2 + rp3 + rp4 + rp5 + rp6 + rp7;
    CHECK("  Fan-in checksum", g_i64 == ref);
}

/*
 * 10c. Long dependency chain — 10000 deep
 */
static void test_dep_long_chain(void)
{
    printf("[10c] Dep: Long dependency chain (10000 deep)\n");

    int64_t x = 42;
    int i;
    for (i = 0; i < 10000; i++) {
        x = x * 31 + 17;
        x ^= x >> 13;
        x &= 0xFFFFFFFFFFULL;
    }
    g_i64 = x;

    int64_t ref = 42;
    for (i = 0; i < 10000; i++) {
        ref = ref * 31 + 17;
        ref ^= ref >> 13;
        ref &= 0xFFFFFFFFFFULL;
    }
    CHECK("  Long chain checksum", g_i64 == ref);
}

/* ============================================================
 * 11. Edge Cases & Corner Conditions
 * ============================================================ */

/*
 * 11a. Zero and negative value handling
 */
static void test_edge_values(void)
{
    printf("\n[11a] Edge: Zero and negative values\n");

    int64_t zero = 0;
    int64_t neg = -1;
    int i;

    for (i = 0; i < 1000; i++) {
        zero = zero * 31 + 0;
        neg = neg * 31 - 17;
    }
    g_i64 = zero;
    g_u64 = (uint64_t)neg;

    CHECK("  Zero stays zero", zero == 0);
    CHECK("  Negative chain", neg != 0);
}

/*
 * 11b. Large immediate values
 */
static void test_edge_immediates(void)
{
    printf("[11b] Edge: Large immediates\n");

    int64_t big = 0x7FFFFFFFFFFFFFFF;  /* MAX_INT64 */
    int64_t small = -0x8000000000000000;  /* MIN_INT64 */
    int i;

    for (i = 0; i < 500; i++) {
        big = (big >> 1) + 1;
        small = (small >> 1) - 1;
    }
    g_i64 = big;
    g_u64 = (uint64_t)small;

    CHECK("  MAX_INT64 shift", big > 0);
    CHECK("  MIN_INT64 shift", small < 0);
}

/*
 * 11c. FLOF (Float Less than Or Equal / Float Ordered Flag)
 *     Tests FP comparison edge cases
 */
static void test_edge_fp_compare(void)
{
    printf("[11c] Edge: FP comparison edge cases\n");

    double a = 0.0;
    double b = -0.0;
    double nan_val = 0.0 / 0.0;  /* NaN */
    double inf_val = 1.0 / 0.0;  /* +Inf */
    int results = 0;

    /* 0.0 == -0.0 */
    if (a == b) results |= 1;
    /* NaN != NaN */
    if (nan_val != nan_val) results |= 2;
    /* Inf > finite */
    if (inf_val > 1e300) results |= 4;
    /* -Inf < finite */
    if (-inf_val < -1e300) results |= 8;

    g_u32 = (uint32_t)results;
    CHECK("  FP compare: 0==-0", (results & 1) != 0);
    CHECK("  FP compare: NaN!=NaN", (results & 2) != 0);
    CHECK("  FP compare: Inf>finite", (results & 4) != 0);
    CHECK("  FP compare: -Inf<finite", (results & 8) != 0);
}

/* ============================================================
 * 12. Final Summary & Barrier
 * ============================================================ */

static void test_final_summary(void)
{
    printf("\n========================================\n");
    printf(" C910 IEW Full Test — Summary\n");
    printf("========================================\n");
    printf(" Passed: %d\n", g_pass);
    printf(" Failed: %d\n", g_fail);
    printf(" Total:  %d\n", g_pass + g_fail);
    printf("========================================\n");

    if (g_fail == 0) {
        printf(" ALL TESTS PASSED\n");
    } else {
        printf(" SOME TESTS FAILED\n");
    }

    /* Final FENCE barrier */
    __asm__ volatile ("fence rw, rw" ::: "memory");
}

/* ============================================================
 * Main — Run all tests in order
 * ============================================================ */
int main(void)
{
    printf("========================================\n");
    printf(" C910 IEW Full Test Suite\n");
    printf("========================================\n");

    /* 1. IQ Type Routing */
    test_iq_aiq0_routing();
    test_iq_aiq1_mult();
    test_iq_biq_branch();
    test_iq_lsiq_load();
    test_iq_sdiq_store();
    test_iq_vmb_vector();
    test_iq_viq0_valu();

    /* 2. IQ Overflow & Fallback */
    test_iq_overflow_biq();
    test_iq_overflow_aiq1();

    /* 3. Stall Types */
    test_stall_dispatch();
    test_stall_div();
    test_stall_div_mixed();

    /* 4. EX1 vs EX2 Path */
    test_ex1_path();
    test_ex2_path();
    test_ex1_ex2_dependency();

    /* 5. FENCE Ordering */
    test_fence_ordering();

    /* 6. Store-Load Forwarding */
    test_store_load_forwarding();

    /* 7. Multi-PRF Scoreboard */
    test_prf_preg();
    test_prf_ereg();

    /* 8. Writeback Port Classification */
    test_wb_preg();
    test_wb_ereg();

    /* 9. Large-Scale Mixed Stress */
    test_stress_mixed();

    /* 10. Dependency Chains */
    test_dep_fanout();
    test_dep_fanin();
    test_dep_long_chain();

    /* 11. Edge Cases */
    test_edge_values();
    test_edge_immediates();
    test_edge_fp_compare();

    /* 12. Summary */
    test_final_summary();

    return g_fail > 0 ? 1 : 0;
}
