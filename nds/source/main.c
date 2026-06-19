/*
 * Nintendo DS uzstd benchmark.
 *
 * This is intentionally self-contained: no filesystem, no NitroFS, and no
 * external ROM assets. Results are printed to the libnds console as CSV.
 */
#include <stddef.h>
#include <stdint.h>

static void *bench_alloc(size_t size);
static void *bench_memcpy(void *dst, const void *src, size_t n);
static void *bench_memset(void *dst, int c, size_t n);
static int bench_memcmp(const void *a, const void *b, size_t n);

#define UZSTD_MALLOC(sz) bench_alloc(sz)
#define UZSTD_FREE(p) ((void)(p))
#define UZSTD_MEMCPY(dst, src, n) bench_memcpy((dst), (src), (n))
#define UZSTD_MEMSET(dst, c, n) bench_memset((dst), (c), (n))
#define UZSTD_MEMCMP(a, b, n) bench_memcmp((a), (b), (n))
#define UZSTD_IMPLEMENTATION
#include "../../uzstd.h"

#define TIMER_HZ 33513982u
#define MAX_SRC 32u * 1024u
#define MAX_COMP UZSTD_COMPRESS_BOUND(MAX_SRC)
#define ARENA_SIZE (2u * 1024u * 1024u)
#define BENCH_MAGIC "UZNDST1"
#define BENCH_VERSION 1u
#define BENCH_CASE_COUNT 5u

#define TIMER0_DATA (*(volatile uint16_t*)0x04000100u)
#define TIMER0_CR   (*(volatile uint16_t*)0x04000102u)
#define TIMER1_DATA (*(volatile uint16_t*)0x04000104u)
#define TIMER1_CR   (*(volatile uint16_t*)0x04000106u)
#define TIMER_ENABLE 0x0080u
#define TIMER_CASCADE 0x0004u
#define TIMER_DIV_1 0x0000u

#ifndef NDS_BENCH_CONSOLE
#define NDS_BENCH_CONSOLE 0
#endif

#define BENCH_PRINT(...) ((void)0)

typedef unsigned char u8_local;

typedef struct BenchCase {
    const char *name;
    size_t size;
    void (*fill)(u8_local *dst, size_t size);
} BenchCase;

typedef struct BenchResult {
    char name[16];
    unsigned src_bytes;
    unsigned comp_bytes;
    unsigned compress_ticks;
    unsigned decompress_ticks;
    unsigned src_hash;
    unsigned comp_hash;
    unsigned dec_hash;
    unsigned status;
} BenchResult;

typedef struct BenchBlock {
    char magic[8];
    unsigned version;
    unsigned done;
    unsigned case_count;
    unsigned failures;
    unsigned result_bytes;
    BenchResult results[BENCH_CASE_COUNT];
} BenchBlock;

static u8_local g_src[MAX_SRC];
static u8_local g_comp[MAX_COMP];
static u8_local g_dec[MAX_SRC];
static u8_local g_arena[ARENA_SIZE] __attribute__((aligned(4)));
static size_t g_arena_pos;

__attribute__((used, aligned(4)))
static volatile BenchBlock g_bench_block = {
    .magic = BENCH_MAGIC,
    .version = BENCH_VERSION,
    .done = 0u,
    .case_count = BENCH_CASE_COUNT,
    .failures = 0u,
    .result_bytes = sizeof(BenchBlock),
    .results = {{{0}}}
};

static unsigned rng_state = 0x12345678u;

static void *bench_memcpy(void *dst, const void *src, size_t n) {
    u8_local *d = (u8_local*)dst;
    const u8_local *s = (const u8_local*)src;
    while (n--)
        *d++ = *s++;
    return dst;
}

static void *bench_memset(void *dst, int c, size_t n) {
    u8_local *d = (u8_local*)dst;
    while (n--)
        *d++ = (u8_local)c;
    return dst;
}

static int bench_memcmp(const void *a, const void *b, size_t n) {
    const u8_local *pa = (const u8_local*)a;
    const u8_local *pb = (const u8_local*)b;
    while (n--) {
        if (*pa != *pb)
            return (int)*pa - (int)*pb;
        pa++;
        pb++;
    }
    return 0;
}

static void arena_reset(void) {
    g_arena_pos = 0;
}

static void *bench_alloc(size_t size) {
    size_t aligned = (size + 3u) & ~(size_t)3u;
    void *p;
    if (aligned > ARENA_SIZE || g_arena_pos > ARENA_SIZE - aligned)
        return NULL;
    p = &g_arena[g_arena_pos];
    g_arena_pos += aligned;
    return p;
}

static unsigned next_rand8(void) {
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state >> 24;
}

static void timer_start(void) {
    TIMER0_CR = 0;
    TIMER1_CR = 0;
    TIMER0_DATA = 0;
    TIMER1_DATA = 0;
    TIMER0_CR = TIMER_ENABLE | TIMER_DIV_1;
    TIMER1_CR = TIMER_ENABLE | TIMER_CASCADE;
}

static unsigned timer_read(void) {
    return ((unsigned)TIMER1_DATA << 16) | (unsigned)TIMER0_DATA;
}

static unsigned fnv1a(const u8_local *data, size_t size) {
    unsigned hash = 2166136261u;
    size_t i;
    for (i = 0; i < size; i++) {
        hash ^= data[i];
        hash *= 16777619u;
    }
    return hash;
}

static void fill_tiny(u8_local *dst, size_t size) {
    static const char text[] = "hello hello hello hello, world world!";
    size_t i;
    for (i = 0; i < size; i++)
        dst[i] = (u8_local)text[i % (sizeof text - 1)];
}

static void fill_repeat(u8_local *dst, size_t size) {
    bench_memset(dst, 'A', size);
}

static void fill_randomish(u8_local *dst, size_t size) {
    size_t i;
    rng_state = 0x12345678u;
    for (i = 0; i < size; i++)
        dst[i] = (u8_local)(next_rand8() ^ (next_rand8() << 3));
}

static void fill_structured(u8_local *dst, size_t size) {
    size_t i;
    rng_state = 0x87654321u;
    for (i = 0; i < size; i += 4) {
        unsigned v = 1000000u + (unsigned)(i / 4) + (next_rand8() & 15u);
        if (i + 0 < size) dst[i + 0] = (u8_local)v;
        if (i + 1 < size) dst[i + 1] = (u8_local)(v >> 8);
        if (i + 2 < size) dst[i + 2] = (u8_local)(v >> 16);
        if (i + 3 < size) dst[i + 3] = (u8_local)(v >> 24);
    }
}

static void fill_header_text(u8_local *dst, size_t size) {
    static const char text[] =
        "/* uzstd.h - micro zstd (RFC 8878) single-header codec. */\n"
        "#ifndef UZSTD_H\n#define UZSTD_H\n#include <stddef.h>\n"
        "UZSTD_LINKAGE size_t uzstd_compress(void *dst, size_t dst_cap, "
        "const void *src, size_t src_size, int level);\n"
        "UZSTD_LINKAGE size_t uzstd_decompress(void *dst, size_t dst_cap, "
        "const void *src, size_t src_size);\n"
        "The encoder emits standard zstd frames. The decoder accepts standard "
        "zstd frames except dictionaries and legacy pre-1.0 formats.\n";
    size_t i;
    for (i = 0; i < size; i++)
        dst[i] = (u8_local)text[i % (sizeof text - 1)];
}

static void store_name(volatile char *dst, const char *src) {
    size_t i;
    for (i = 0; i < 15 && src[i]; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

static void run_case(const BenchCase *bc, size_t index) {
    size_t csize, dsize;
    unsigned c_ticks, d_ticks;
    volatile BenchResult *result = &g_bench_block.results[index];

    bc->fill(g_src, bc->size);
    store_name(result->name, bc->name);
    result->src_bytes = (unsigned)bc->size;
    result->src_hash = fnv1a(g_src, bc->size);

    arena_reset();
    timer_start();
    csize = uzstd_compress(g_comp, MAX_COMP, g_src, bc->size, 3);
    c_ticks = timer_read();
    result->compress_ticks = c_ticks;

    if (UZSTD_IS_ERROR(csize)) {
        result->status = 0u;
        g_bench_block.failures++;
        BENCH_PRINT("%s,%lu,ERR,ERR,ERR,FAIL\n", bc->name, (unsigned long)bc->size);
        return;
    }
    result->comp_bytes = (unsigned)csize;
    result->comp_hash = fnv1a(g_comp, csize);

    arena_reset();
    timer_start();
    dsize = uzstd_decompress(g_dec, MAX_SRC, g_comp, csize);
    d_ticks = timer_read();
    result->decompress_ticks = d_ticks;

    if (!UZSTD_IS_ERROR(dsize))
        result->dec_hash = fnv1a(g_dec, dsize);

    if (UZSTD_IS_ERROR(dsize) || dsize != bc->size || bench_memcmp(g_src, g_dec, bc->size) != 0) {
        result->status = 0u;
        g_bench_block.failures++;
    } else {
        result->status = 1u;
    }

    BENCH_PRINT("%s,%lu,%lu,%lu,%lu,%s\n",
                bc->name,
                (unsigned long)bc->size,
                (unsigned long)csize,
                (unsigned long)c_ticks,
                (unsigned long)d_ticks,
                result->status ? "ok" : "FAIL");
}

int main(void) {
    static const BenchCase cases[BENCH_CASE_COUNT] = {
        {"tiny", 37, fill_tiny},
        {"repeat", 8192, fill_repeat},
        {"randomish", 8192, fill_randomish},
        {"header_text", 8192, fill_header_text},
        {"structured", 32768, fill_structured},
    };
    size_t i;

    BENCH_PRINT("uzstd Nintendo DS benchmark\n");
    BENCH_PRINT("timer_hz,%lu\n", (unsigned long)TIMER_HZ);
    BENCH_PRINT("case,src_bytes,comp_bytes,compress_ticks,decompress_ticks,status\n");

    for (i = 0; i < sizeof cases / sizeof cases[0]; i++)
        run_case(&cases[i], i);

    g_bench_block.done = 1u;
    BENCH_PRINT("DONE\n");

    for (;;)
        __asm__ volatile("nop");
}
