/*
 * c910_bench.c — 复杂确定性计算基准程序
 *
 * 目标：运行一组确定性计算算法，输出可验证的 checksum。
 * gem5 Before/After 修改后，输出结果应该完全一致（计算正确性），
 * 但运行周期数和内部 stall 统计不同（性能差异）。
 *
 * 包含算法：
 *   1. Matrix Multiply (16x16) — 整数矩阵乘法
 *   2. SHA-256 计算（固定字符串） — 密码学哈希
 *   3. Quick Sort（10000 元素） — 排序
 *   4. Fibonacci（前 46 项） — 递推计算
 *   5. CRC32（大缓冲区） — 循环冗余校验
 *   6. MD5-like Hash（自定义） — 混叠哈希
 *   7. Sieve of Eratosthenes（找素数） — 筛法
 *   8. LCG Random（伪随机序列） — 确定性随机
 *
 * 编译：riscv64-linux-gnu-gcc -O2 -static -o c910_bench c910_bench.c
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

/* ============================================================
 * 1. Matrix Multiply — 16x16 整数矩阵
 * ============================================================ */
#define MAT_N 16

static void mat_mul(uint64_t A[MAT_N][MAT_N],
                    uint64_t B[MAT_N][MAT_N],
                    uint64_t C[MAT_N][MAT_N])
{
    int i, j, k;
    memset(C, 0, sizeof(*C) * MAT_N);
    for (i = 0; i < MAT_N; i++) {
        for (k = 0; k < MAT_N; k++) {
            uint64_t r = A[i][k];
            for (j = 0; j < MAT_N; j++)
                C[i][j] += r * B[k][j];
        }
    }
}

static void test_matrix(void)
{
    uint64_t A[MAT_N][MAT_N], B[MAT_N][MAT_N], C[MAT_N][MAT_N];
    uint64_t sum = 0;
    int i, j;

    /* 确定性初始化 */
    for (i = 0; i < MAT_N; i++)
        for (j = 0; j < MAT_N; j++) {
            A[i][j] = (uint64_t)(i * MAT_N + j) * 7 + 3;
            B[i][j] = (uint64_t)(i + j * MAT_N) * 13 + 11;
        }

    /* 做 100 次矩阵乘法 */
    for (i = 0; i < 100; i++)
        mat_mul(A, B, C);

    for (i = 0; i < MAT_N; i++)
        for (j = 0; j < MAT_N; j++)
            sum += C[i][j];

    printf("[1] MatrixMul(16x16, 100x): checksum = 0x%016lx\n",
           (unsigned long)sum);
}

/* ============================================================
 * 2. SHA-256（完整实现）
 * ============================================================ */
#define ROTR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
#define CH(x, y, z)  (((x) & (y)) ^ (~(x) & (z)))
#define MAJ(x, y, z) (((x) & (y)) ^ ((x) & (z)) ^ ((y) & (z)))
#define EP0(x) (ROTR(x, 2) ^ ROTR(x, 13) ^ ROTR(x, 22))
#define EP1(x) (ROTR(x, 6) ^ ROTR(x, 11) ^ ROTR(x, 25))
#define SIG0(x) (ROTR(x, 7) ^ ROTR(x, 18) ^ ((x) >> 3))
#define SIG1(x) (ROTR(x, 17) ^ ROTR(x, 19) ^ ((x) >> 10))

static const uint32_t sha256_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

static void sha256_hash(const uint8_t *msg, size_t len, uint8_t out[32])
{
    uint32_t h[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };
    uint8_t buf[64];
    uint32_t w[64];
    uint32_t a, b, c, d, e, f, g, hh;
    size_t i, offset = 0;
    uint64_t bitlen = len * 8;

    while (len > 64) {
        memcpy(buf, msg + offset, 64);
        offset += 64;
        len -= 64;

        for (i = 0; i < 16; i++)
            w[i] = (buf[i*4]<<24) | (buf[i*4+1]<<16) | (buf[i*4+2]<<8) | buf[i*4+3];
        for (i = 16; i < 64; i++)
            w[i] = SIG1(w[i-2]) + w[i-7] + SIG0(w[i-15]) + w[i-16];

        a=h[0]; b=h[1]; c=h[2]; d=h[3];
        e=h[4]; f=h[5]; g=h[6]; hh=h[7];

        for (i = 0; i < 64; i++) {
            uint32_t t1 = hh + EP1(e) + CH(e,f,g) + sha256_k[i] + w[i];
            uint32_t t2 = EP0(a) + MAJ(a,b,c);
            hh=g; g=f; f=e; d=c; c=b; b=a; a=t1+t2; e=d+t1;
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d;
        h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
    }

    /* 最后一个块 */
    memset(buf, 0, 64);
    memcpy(buf, msg + offset, len);
    buf[len] = 0x80;
    if (len < 56) {
        buf[56] = (bitlen >> 56) & 0xff;
        buf[57] = (bitlen >> 48) & 0xff;
        buf[58] = (bitlen >> 40) & 0xff;
        buf[59] = (bitlen >> 32) & 0xff;
        buf[60] = (bitlen >> 24) & 0xff;
        buf[61] = (bitlen >> 16) & 0xff;
        buf[62] = (bitlen >>  8) & 0xff;
        buf[63] = (bitlen      ) & 0xff;
    }

    /* 简化处理：只做一次完整块哈希 */
    for (i = 0; i < 16; i++)
        w[i] = (buf[i*4]<<24) | (buf[i*4+1]<<16) | (buf[i*4+2]<<8) | buf[i*4+3];
    for (i = 16; i < 64; i++)
        w[i] = SIG1(w[i-2]) + w[i-7] + SIG0(w[i-15]) + w[i-16];

    a=h[0]; b=h[1]; c=h[2]; d=h[3];
    e=h[4]; f=h[5]; g=h[6]; hh=h[7];

    for (i = 0; i < 64; i++) {
        uint32_t t1 = hh + EP1(e) + CH(e,f,g) + sha256_k[i] + w[i];
        uint32_t t2 = EP0(a) + MAJ(a,b,c);
        hh=g; g=f; f=e; d=c; c=b; b=a; a=t1+t2; e=d+t1;
    }
    h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d;
    h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;

    for (i = 0; i < 8; i++) {
        out[i*4]   = (h[i] >> 24) & 0xff;
        out[i*4+1] = (h[i] >> 16) & 0xff;
        out[i*4+2] = (h[i] >>  8) & 0xff;
        out[i*4+3] = (h[i]      ) & 0xff;
    }
}

static void test_sha256(void)
{
    const char *msg = "The quick brown fox jumps over the lazy dog";
    uint8_t hash[32];
    int i;

    /* 计算 10000 次 SHA-256 */
    for (i = 0; i < 10000; i++)
        sha256_hash((const uint8_t *)msg, strlen(msg), hash);

    printf("[2] SHA-256(10000x): ");
    for (i = 0; i < 8; i++)
        printf("%02x", hash[i]);
    printf("...\n");
}

/* ============================================================
 * 3. Quick Sort — 10000 元素
 * ============================================================ */
static void swap_int(int *a, int *b) { int t = *a; *a = *b; *b = t; }

static void quick_sort(int *arr, int lo, int hi)
{
    if (lo >= hi) return;
    int pivot = arr[hi];
    int i = lo, j;
    for (j = lo; j < hi; j++) {
        if (arr[j] <= pivot) {
            swap_int(&arr[i], &arr[j]);
            i++;
        }
    }
    swap_int(&arr[i], &arr[hi]);
    quick_sort(arr, lo, i - 1);
    quick_sort(arr, i + 1, hi);
}

static void test_quicksort(void)
{
    int arr[10000];
    uint64_t sum = 0;
    int i;

    /* 确定性 LCG 填充 */
    uint64_t seed = 12345;
    for (i = 0; i < 10000; i++) {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        arr[i] = (int)(seed & 0x7FFFFFFF);
    }

    /* 排序 10 次 */
    for (i = 0; i < 10; i++) {
        /* 每次恢复原始数据 */
        seed = 12345;
        int j;
        for (j = 0; j < 10000; j++) {
            seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
            arr[j] = (int)(seed & 0x7FFFFFFF);
        }
        quick_sort(arr, 0, 9999);
    }

    for (i = 0; i < 10000; i++)
        sum += (uint64_t)arr[i];

    /* 验证有序 */
    int sorted = 1;
    for (i = 0; i < 9999; i++)
        if (arr[i] > arr[i+1]) { sorted = 0; break; }

    printf("[3] QuickSort(10000, 10x): checksum = 0x%016lx, sorted = %d\n",
           (unsigned long)sum, sorted);
}

/* ============================================================
 * 4. Fibonacci — 递推
 * ============================================================ */
static void test_fibonacci(void)
{
    uint64_t fib[47];
    uint64_t sum = 0;
    int i;

    fib[0] = 0;
    fib[1] = 1;
    for (i = 2; i < 47; i++)
        fib[i] = fib[i-1] + fib[i-2];

    for (i = 0; i < 47; i++)
        sum += fib[i];

    printf("[4] Fibonacci(0..46): sum = %lu, fib[46] = %lu\n",
           (unsigned long)sum, (unsigned long)fib[46]);
}

/* ============================================================
 * 5. CRC32 — 大缓冲区
 * ============================================================ */
static uint32_t crc32_table[256];
static int crc32_table_init = 0;

static void init_crc32(void)
{
    uint32_t c;
    int i, j;
    for (i = 0; i < 256; i++) {
        c = (uint32_t)i;
        for (j = 0; j < 8; j++)
            c = (c >> 1) ^ ((c & 1) ? 0xEDB88320 : 0);
        crc32_table[i] = c;
    }
    crc32_table_init = 1;
}

static uint32_t calc_crc32(const uint8_t *buf, size_t len)
{
    uint32_t crc = 0xFFFFFFFF;
    size_t i;
    for (i = 0; i < len; i++)
        crc = (crc >> 8) ^ crc32_table[(crc ^ buf[i]) & 0xFF];
    return crc ^ 0xFFFFFFFF;
}

static void test_crc32(void)
{
    uint8_t buf[4096];
    uint32_t crc;
    int i;

    if (!crc32_table_init)
        init_crc32();

    /* 确定性填充 */
    uint64_t seed = 99999;
    for (i = 0; i < 4096; i++) {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        buf[i] = (uint8_t)(seed & 0xFF);
    }

    /* 计算 CRC 100 次 */
    for (i = 0; i < 100; i++)
        crc = calc_crc32(buf, 4096);

    printf("[5] CRC32(4096B, 100x): checksum = 0x%08x\n", (unsigned)crc);
}

/* ============================================================
 * 6. Sieve of Eratosthenes — 找素数
 * ============================================================ */
static void test_sieve(void)
{
    #define N 100000
    static uint8_t is_prime[N];
    int count = 0, last = 0, i, j;

    memset(is_prime, 1, sizeof(is_prime));
    is_prime[0] = is_prime[1] = 0;

    for (i = 2; i * i < N; i++) {
        if (is_prime[i]) {
            for (j = i * i; j < N; j += i)
                is_prime[j] = 0;
        }
    }

    for (i = 0; i < N; i++) {
        if (is_prime[i]) {
            count++;
            last = i;
        }
    }

    printf("[6] Sieve(100000): count = %d, last_prime = %d\n", count, last);
}

/* ============================================================
 * 7. LCG Random — 长序列确定性随机
 * ============================================================ */
static void test_lcg(void)
{
    uint64_t x = 42;
    uint64_t sum = 0;
    int i;

    /* 1000000 次 LCG */
    for (i = 0; i < 1000000; i++) {
        x = x * 6364136223846793005ULL + 1442695040888963407ULL;
        sum += x;
    }

    printf("[7] LCG(1000000): sum = 0x%016lx\n", (unsigned long)sum);
}

/* ============================================================
 * 8. Prime factorization — 大量整数分解
 * ============================================================ */
static void test_factorization(void)
{
    uint64_t total = 0;
    int i;

    /* 分解 100000 个连续整数的质因数之和 */
    for (i = 1000000; i < 1100000; i++) {
        int n = i;
        int psum = 0;
        int d;
        for (d = 2; d * d <= n; d++) {
            while (n % d == 0) {
                psum += d;
                n /= d;
            }
        }
        if (n > 1)
            psum += n;
        total += psum;
    }

    printf("[8] Factorization(100000 nums): sum_factors = %lu\n",
           (unsigned long)total);
}

/* ============================================================
 * 9. String Hash（多种哈希算法混合）
 * ============================================================ */
static void test_string_hash(void)
{
    const char *s = "The quick brown fox jumps over the lazy dog";
    uint64_t h1 = 0, h2 = 0, h3 = 0;
    int i, len = strlen(s);

    /* DJB2 */
    h1 = 5381;
    for (i = 0; i < len; i++)
        h1 = ((h1 << 5) + h1) + s[i];

    /* FNV-1a */
    h2 = 0x811c9dc5;
    for (i = 0; i < len; i++) {
        h2 ^= (uint8_t)s[i];
        h2 *= 0x01000193;
    }

    /* Jenkins One-at-a-time */
    h3 = 0;
    for (i = 0; i < len; i++) {
        h3 += s[i];
        h3 += (h3 << 10);
        h3 ^= (h3 >> 6);
    }
    h3 += (h3 << 3);
    h3 ^= (h3 >> 11);
    h3 += (h3 << 15);

    /* 各做 100000 次 */
    for (i = 0; i < 100000; i++) {
        /* DJB2 */
        uint64_t t1 = 5381;
        int j;
        for (j = 0; j < len; j++)
            t1 = ((t1 << 5) + t1) + s[j];
        h1 ^= t1;

        /* FNV-1a */
        uint32_t t2 = 0x811c9dc5;
        for (j = 0; j < len; j++) {
            t2 ^= (uint8_t)s[j];
            t2 *= 0x01000193;
        }
        h2 ^= t2;
    }

    printf("[9] StringHash(DJB2^FNV^Jenkins, 100kx): h = 0x%016lx\n",
           (unsigned long)(h1 ^ h2 ^ h3));
}

/* ============================================================
 * 主函数
 * ============================================================ */
int main(void)
{
    printf("========================================\n");
    printf(" C910 Complex Computation Benchmark\n");
    printf("========================================\n\n");

    test_matrix();
    test_sha256();
    test_quicksort();
    test_fibonacci();
    test_crc32();
    test_sieve();
    test_lcg();
    test_factorization();
    test_string_hash();

    printf("\n========================================\n");
    printf(" All tests completed.\n");
    printf("========================================\n");
    return 0;
}
