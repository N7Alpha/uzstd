/*
 * uzstd forever fuzzer.
 *
 * Build:
 *   cc -std=c99 -O2 -g -Wall -Wextra -pedantic tools/fuzz_forever.c -o uzstd_fuzz
 *
 * Useful sanitizer build:
 *   cc -std=c99 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
 *      -Wall -Wextra -pedantic tools/fuzz_forever.c -o uzstd_fuzz_asan
 *
 * Run:
 *   ./uzstd_fuzz
 *
 * Environment:
 *   UZSTD_FUZZ_DIR          artifact directory, default ./uzstd_fuzz_artifacts
 *   UZSTD_FUZZ_MAX_CRASHES  worker crashes before stopping, default 10
 *   UZSTD_FUZZ_MAX_SIZE     largest generated input, default 524288
 *   UZSTD_FUZZ_MAX_ITERS    worker iterations before clean exit, default 0 forever
 *   UZSTD_FUZZ_LOG_EVERY    progress interval, default 10000
 *   UZSTD_FUZZ_SEED         starting seed, default time/pid derived
 */

#include <stddef.h>

/* Fault-injecting allocator so coverage can reach the out-of-memory branches.
** g_alloc_fail_after: <0 never fails; otherwise the Nth UZSTD_MALLOC returns NULL
** (0 = the very next one), then it disarms itself. */
static long g_alloc_fail_after = -1;
static void *fuzz_malloc(size_t n);
static void fuzz_free(void *p);
#define UZSTD_MALLOC(n) fuzz_malloc(n)
#define UZSTD_FREE(p) fuzz_free(p)

#define UZSTD_IMPLEMENTATION
#include "../uzstd.h"

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef _WIN32
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#else
#error "tools/fuzz_forever.c needs POSIX fork/waitpid/setrlimit supervision."
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

typedef struct {
    uint64_t s;
} FuzzRng;

typedef struct {
    unsigned long long iter;
    uint64_t seed;
    size_t size;
    int pattern;
    int level;
} FuzzCase;

typedef struct {
    char dir[PATH_MAX];
    unsigned max_crashes;
    unsigned long long max_iters;
    unsigned long long log_every;
    size_t max_size;
    uint64_t seed;
} FuzzConfig;

static void *fuzz_malloc(size_t n) {
    if (g_alloc_fail_after >= 0) {
        if (g_alloc_fail_after == 0) { g_alloc_fail_after = -1; return 0; }
        g_alloc_fail_after--;
    }
    return malloc(n);
}

static void fuzz_free(void *p) {
    free(p);
}

static uint64_t rng_next(FuzzRng *r) {
    uint64_t x = r->s;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    r->s = x ? x : 0x9e3779b97f4a7c15ull;
    return r->s * 2685821657736338717ull;
}

static unsigned rng_u32(FuzzRng *r) {
    return (unsigned)(rng_next(r) >> 32);
}

static size_t rng_range(FuzzRng *r, size_t n) {
    return n ? (size_t)(rng_next(r) % n) : 0;
}

static unsigned long long env_ull(const char *name, unsigned long long def) {
    const char *s = getenv(name);
    char *end = 0;
    unsigned long long v;
    if (!s || !s[0])
        return def;
    errno = 0;
    v = strtoull(s, &end, 0);
    return errno || !end || *end ? def : v;
}

static void join_path(char *out, size_t out_cap, const char *dir, const char *name) {
    size_t n = strlen(dir);
    snprintf(out, out_cap, "%s%s%s", dir, (n && dir[n - 1] == '/') ? "" : "/", name);
}

static void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(2);
}

static void mkdir_p_one(const char *path) {
    if (mkdir(path, 0777) != 0 && errno != EEXIST)
        die("mkdir %s: %s", path, strerror(errno));
}

static void write_file(const char *path, const void *data, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f)
        die("open %s: %s", path, strerror(errno));
    if (n && fwrite(data, 1, n, f) != n) {
        fclose(f);
        die("write %s: %s", path, strerror(errno));
    }
    if (fclose(f) != 0)
        die("close %s: %s", path, strerror(errno));
}

static void write_current_case(const FuzzConfig *cfg, const FuzzCase *fc,
                               const unsigned char *src) {
    char path[PATH_MAX], tmp[PATH_MAX], meta[PATH_MAX], metatmp[PATH_MAX];
    char text[512];
    int n;

    join_path(path, sizeof path, cfg->dir, "current.bin");
    join_path(tmp, sizeof tmp, cfg->dir, "current.bin.tmp");
    write_file(tmp, src, fc->size);
    if (rename(tmp, path) != 0)
        die("rename %s -> %s: %s", tmp, path, strerror(errno));

    join_path(meta, sizeof meta, cfg->dir, "current.txt");
    join_path(metatmp, sizeof metatmp, cfg->dir, "current.txt.tmp");
    n = snprintf(text, sizeof text,
                 "iter=%llu\nseed=%llu\nsize=%llu\npattern=%d\nlevel=%d\n",
                 fc->iter, (unsigned long long)fc->seed,
                 (unsigned long long)fc->size, fc->pattern, fc->level);
    if (n < 0 || (size_t)n >= sizeof text)
        die("metadata overflow");
    write_file(metatmp, text, (size_t)n);
    if (rename(metatmp, meta) != 0)
        die("rename %s -> %s: %s", metatmp, meta, strerror(errno));
}

static void copy_file_if_present(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb");
    FILE *out;
    unsigned char buf[8192];
    size_t n;
    if (!in)
        return;
    out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        return;
    }
    while ((n = fread(buf, 1, sizeof buf, in)) != 0)
        (void)fwrite(buf, 1, n, out);
    fclose(out);
    fclose(in);
}

static void save_named_case(const FuzzConfig *cfg, const char *prefix,
                            unsigned index, const unsigned char *src,
                            size_t n, const FuzzCase *fc) {
    char name[64], path[PATH_MAX], meta_name[64], meta[PATH_MAX], text[512];
    int len;
    snprintf(name, sizeof name, "%s_%02u.bin", prefix, index);
    join_path(path, sizeof path, cfg->dir, name);
    write_file(path, src, n);
    snprintf(meta_name, sizeof meta_name, "%s_%02u.txt", prefix, index);
    join_path(meta, sizeof meta, cfg->dir, meta_name);
    len = snprintf(text, sizeof text,
                   "iter=%llu\nseed=%llu\nsize=%llu\npattern=%d\nlevel=%d\n",
                   fc->iter, (unsigned long long)fc->seed,
                   (unsigned long long)fc->size, fc->pattern, fc->level);
    if (len > 0)
        write_file(meta, text, (size_t)len);
}

static size_t choose_size(FuzzRng *r, size_t max_size) {
    static const size_t edges[] = {
        0, 1, 2, 3, 4, 5, 7, 8, 15, 16, 17, 31, 32, 33, 63, 64, 65,
        127, 128, 129, 255, 256, 257, 511, 512, 513, 1023, 1024, 1025,
        4095, 4096, 4097, 8191, 8192, 8193, 32767, 32768, 32769,
        65535, 65536, 65537, 131071, 131072, 131073
    };
    unsigned mode = rng_u32(r) % 10u;
    size_t n;
    if (mode < 4) {
        n = edges[rng_range(r, sizeof edges / sizeof edges[0])];
    } else if (mode < 7) {
        n = rng_range(r, max_size < 4096 ? max_size + 1 : 4097);
    } else {
        n = rng_range(r, max_size + 1);
    }
    return n > max_size ? max_size : n;
}

static void fill_case(FuzzRng *r, unsigned char *dst, size_t n, int pattern) {
    size_t i;
    switch (pattern) {
    case 0:
        memset(dst, 0, n);
        break;
    case 1:
        memset(dst, (int)(rng_u32(r) & 255u), n);
        break;
    case 2:
        for (i = 0; i < n; i++)
            dst[i] = (unsigned char)i;
        break;
    case 3:
        for (i = 0; i < n; i++)
            dst[i] = (unsigned char)(rng_u32(r) >> 24);
        break;
    case 4: {
        unsigned alphabet = 1u + (rng_u32(r) % 8u);
        for (i = 0; i < n; i++)
            dst[i] = (unsigned char)('a' + (rng_u32(r) % alphabet));
        break;
    }
    case 5: {
        size_t p = 0;
        while (p < n) {
            unsigned char v = (unsigned char)(rng_u32(r) >> 24);
            size_t run = 1 + rng_range(r, 256);
            if (run > n - p)
                run = n - p;
            memset(dst + p, v, run);
            p += run;
        }
        break;
    }
    case 6: {
        static const unsigned char text[] =
            "hello hello hello hello, world world! "
            "the quick brown fox jumps over the lazy dog. "
            "uzstd fuzz coverage data ";
        for (i = 0; i < n; i++)
            dst[i] = text[(i + rng_range(r, sizeof text - 1)) % (sizeof text - 1)];
        break;
    }
    case 7:
        for (i = 0; i + 4 <= n; i += 4) {
            uint32_t v = (uint32_t)(1000000u + (unsigned)(i >> 2) + (rng_u32(r) & 15u));
            dst[i + 0] = (unsigned char)v;
            dst[i + 1] = (unsigned char)(v >> 8);
            dst[i + 2] = (unsigned char)(v >> 16);
            dst[i + 3] = (unsigned char)(v >> 24);
        }
        while (i < n)
            dst[i++] = (unsigned char)(rng_u32(r) >> 24);
        break;
    case 8:
        memset(dst, 0, n);
        for (i = 0; i < n / 16 + 1; i++)
            if (n)
                dst[rng_range(r, n)] = (unsigned char)(1 + (rng_u32(r) & 255u));
        break;
    default:
        for (i = 0; i < n; i++) {
            if (i >= 64 && (rng_u32(r) & 7u) == 0)
                dst[i] = dst[i - 1 - rng_range(r, 64)];
            else
                dst[i] = (unsigned char)((i * 17u + (i >> 3) + rng_u32(r)) & 255u);
        }
        break;
    }
}

static void mutate_bytes(FuzzRng *r, unsigned char *buf, size_t n) {
    unsigned edits = 1u + (rng_u32(r) % 12u);
    unsigned e;
    if (!n)
        return;
    for (e = 0; e < edits; e++) {
        size_t p = rng_range(r, n);
        switch (rng_u32(r) % 4u) {
        case 0:
            buf[p] ^= (unsigned char)(1u << (rng_u32(r) & 7u));
            break;
        case 1:
            buf[p] = (unsigned char)(rng_u32(r) >> 24);
            break;
        case 2:
            memset(buf + p, 0, 1 + rng_range(r, n - p));
            break;
        default:
            buf[p] = 0xffu;
            break;
        }
    }
}

static void expect_decode_error(const unsigned char *buf, size_t n) {
    unsigned char out[64];
    (void)uzstd_frame_content_size(buf, n);
    if (!UZSTD_IS_ERROR(uzstd_decompress(out, sizeof out, buf, n)))
        die("malformed frame unexpectedly decoded");
}

static void fuzz_wle64(unsigned char *p, uint64_t v) {
    uzstd__wle32(p, (uzstd__u32)v);
    uzstd__wle32(p + 4, (uzstd__u32)(v >> 32));
}

static void run_malformed_frame_seeds(void) {
    unsigned char buf[128], out[32], empty[32];
    size_t csz;

    csz = uzstd_compress(empty, sizeof empty, "", 0, 5);
    if (UZSTD_IS_ERROR(csz))
        die("empty seed compression failed");

    UZSTD_MEMSET(buf, 0, sizeof buf);
    uzstd__wle32(buf, 0x184D2A50u);
    uzstd__wle32(buf + 4, 3);
    buf[8] = 's';
    buf[9] = 'k';
    buf[10] = 'p';
    UZSTD_MEMCPY(buf + 11, empty, csz);
    if (uzstd_frame_content_size(buf, 11 + csz) != 0)
        die("skippable frame content size seed failed");
    if (uzstd_decompress(out, sizeof out, buf, 11 + csz) != 0)
        die("skippable frame decode seed failed");

    uzstd__wle32(buf, 0x184D2A50u);
    uzstd__wle32(buf + 4, 100);
    expect_decode_error(buf, 8);

    UZSTD_MEMSET(buf, 0, sizeof buf);
    uzstd__wle32(buf, 0xFD2FB528u);
    buf[4] = 0x08u;
    expect_decode_error(buf, 5);

    UZSTD_MEMSET(buf, 0, sizeof buf);
    uzstd__wle32(buf, 0xFD2FB528u);
    buf[4] = 0x01u;
    buf[5] = 0;
    buf[6] = 1;
    expect_decode_error(buf, 7);

    UZSTD_MEMSET(buf, 0, sizeof buf);
    uzstd__wle32(buf, 0xFD2FB528u);
    buf[4] = 0x60u;
    fuzz_wle64(buf + 5, 0x100000000ull);
    (void)uzstd_frame_content_size(buf, 13);
    expect_decode_error(buf, 13);

    UZSTD_MEMCPY(buf, empty, csz);
    buf[4] = (unsigned char)(buf[4] | 0x04u);
    UZSTD_MEMSET(buf + csz, 0, 4);
    if (uzstd_decompress(out, sizeof out, buf, csz + 4) != 0)
        die("checksum seed failed");
}

static void run_internal_coverage_seeds(void) {
    static const uzstd__u32 repair_counts[] = {
        1,706192,20,13,904487,106398,14,1,14,15,13,7,14,20,8,387348,
        373218,16,16,11,5,901541,8,2,3,13,476589,13,12,2,7,8,
        13,19,16,242618,2,18,1,448728,748744,20,356639,6,3,14,19,8,
        20,3,16,310510,8,6,1,9,12,13,4,10,75947,12,979245,440837,
        14,294306,13,18,4,578160,9,408162,10,7,15,3,17,19,9,16,
        9,518655,14,19,1,6,2,16,9,670277,168717,137942,4,18,15,2,
        4,8,3,6,17,708979,8,20,599384,12,15,9,11,257900,3,782882,
        29777,11,6,17,1,5,10,12,271868,11,11,19,18,20,11,531501,
        684138,4,18,18,5,590056,19,19,4,12,13,348583,1,19,20,19,
        17,3,141894,3,3,15,19,6,9,14,17,3,404473,7,5,4,5,
        4,995723,13,19,9,7,14,17,20,15,8,7,206666,17,20,11,
        1,511633,4,626436,9,17,7,9,17,920608,5,351037,16,9,121404,
        16,4,9,7,1,2,12,18,13,20,53548,4,20236,18,3,751550,
        13,2,14,18,15,11,15,17,1,768890,9,16,806834,15,6,488931,
        10,232340,17,135391,12,20,18,9,5,13,5,15,15,9,3,14,
        162554,11
    };
    uzstd__ctx cx;
    uzstd__u8 *lit = 0, *tmp = 0, *seqbuf = 0;
    uzstd__seq *seqs = 0;
    uzstd__u8 fh[16], htree[256], hlen[256], hsrc[8], hcode_lens[256];
    uzstd__u16 hcodes[256];
    uzstd__u32 cnt[256];
    uzstd__dctx dcx;
    const uzstd__u8 *litp;
    size_t litsz;
    int last_sym = 0, maxbits;
    size_t i;

    (void)uzstd__frame_header(fh, 0x100000000ull);

    UZSTD_MEMSET(&cx, 0, sizeof cx);
    lit = (uzstd__u8 *)malloc(8192);
    tmp = (uzstd__u8 *)malloc(8192);
    if (!lit || !tmp)
        die("internal coverage allocation failed");

    UZSTD_MEMSET(lit, 'A', 8192);
    cx.lit = lit;
    cx.tmp = tmp;
    cx.nlit = 1000;
    if (!uzstd__lit_section(&cx, tmp, 8192))
        die("literal RLE 12-bit seed failed");
    cx.nlit = 5000;
    if (!uzstd__lit_section(&cx, tmp, 8192))
        die("literal RLE 20-bit seed failed");

    UZSTD_MEMSET(hcode_lens, 0, sizeof hcode_lens);
    hcode_lens[0] = 1;
    hcode_lens[1] = 1;
    if (!uzstd__huf_tree(htree, sizeof htree, hcode_lens, 1, 1))
        die("direct Huffman tree seed failed");

    UZSTD_MEMSET(cnt, 0, sizeof cnt);
    cnt[0] = 1;
    cnt[1] = 1;
    cnt[255] = 1;
    maxbits = uzstd__huf_lengths(cnt, &last_sym, hlen);
    (void)uzstd__huf_canonical(hlen, hcodes, last_sym, maxbits);
    hsrc[0] = 0;
    hsrc[1] = 1;
    hsrc[2] = 255;
    (void)uzstd__huf_stream(htree, htree + 1, hsrc, 3, hcodes, hlen);

    UZSTD_MEMSET(cnt, 0, sizeof cnt);
    for (i = 0; i < sizeof repair_counts / sizeof repair_counts[0]; i++)
        cnt[i] = repair_counts[i];
    (void)uzstd__huf_lengths(cnt, &last_sym, hlen);

    /* histogram (sum=995) that forces the Huffman K<C "shorten most frequent"
    ** repair path after capping+lengthening overshoots below the Kraft target */
    {
        static const uzstd__u32 klc_counts[] = {
            0,89,0,32,0,0,0,15,0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,1,0,0,7,0,0,0,4,
            0,0,0,204,0,0,433,0,0,0,0,42,0,0,0,0,0,0,0,0,0,0,0,0,1,0,1,164,0,1
        };
        UZSTD_MEMSET(cnt, 0, sizeof cnt);
        for (i = 0; i < sizeof klc_counts / sizeof klc_counts[0]; i++)
            cnt[i] = klc_counts[i];
        maxbits = uzstd__huf_lengths(cnt, &last_sym, hlen);
        (void)uzstd__huf_canonical(hlen, hcodes, last_sym, maxbits);
    }

    seqs = (uzstd__seq *)malloc(0x7F00u * sizeof *seqs);
    seqbuf = (uzstd__u8 *)malloc(1u << 20);
    if (!seqs || !seqbuf)
        die("large sequence seed allocation failed");
    for (i = 0; i < 0x7F00u; i++) {
        seqs[i].ll = 0;
        seqs[i].ml = 3;
        seqs[i].ov = 4;
    }
    cx.seqs = seqs;
    cx.nseq = 0x7F00;
    if (!uzstd__encode_sequences(&cx, seqbuf, seqbuf + (1u << 20)))
        die("large sequence seed failed");

    UZSTD_MEMSET(&dcx, 0, sizeof dcx);
    htree[0] = 129;
    htree[1] = 0x11;
    (void)uzstd__huf_tree_d(&dcx, htree, 2);

    htree[0] = (uzstd__u8)(1u | (5u << 3));
    htree[1] = 'Z';
    (void)uzstd__lits_d(&dcx, htree, 2, &litp, &litsz);

    (void)uzstd__seq_prep(&dcx, 0, 0, htree, 0);
    dcx.tok[0] = 0;
    (void)uzstd__seq_prep(&dcx, 0, 3, htree, 0);
    dcx.tok[0] = 1;
    (void)uzstd__seq_prep(&dcx, 0, 3, htree, 0);

    {
        uzstd__u8 block[32];
        uzstd__u8 out[16];
        uzstd__u8 *op = out;
        UZSTD_MEMSET(block, 0, sizeof block);
        block[8] = 0;
        block[9] = 0xff;
        block[10] = 0;
        block[11] = 0;
        block[12] = 0;
        block[13] = 0x80;
        UZSTD_MEMSET(&dcx, 0, sizeof dcx);
        (void)uzstd__block_d(&dcx, &op, out + sizeof out, out, block + 8, 6);
    }

    free(seqbuf);
    free(seqs);
    free(tmp);
    free(lit);
}

/* Force the out-of-memory branches in uzstd_compress and uzstd_decompress via
** the fault-injecting allocator. */
static void run_alloc_fail_seeds(void) {
    unsigned char in[4096], out[8192], dec[4096];
    size_t csz, i;

    for (i = 0; i < sizeof in; i++)
        in[i] = (unsigned char)"abcdabcd abcd abcd "[i % 19];

    g_alloc_fail_after = -1;
    csz = uzstd_compress(out, sizeof out, in, sizeof in, 5);
    if (UZSTD_IS_ERROR(csz))
        die("alloc-fail seed: baseline compression failed");

    g_alloc_fail_after = 0; /* fail compressor's workspace allocation */
    if (!UZSTD_IS_ERROR(uzstd_compress(out, sizeof out, in, sizeof in, 5)))
        die("compressor did not report allocation failure");

    g_alloc_fail_after = 0; /* fail decoder's context allocation */
    if (!UZSTD_IS_ERROR(uzstd_decompress(dec, sizeof dec, out, csz)))
        die("decompressor did not report allocation failure");

    g_alloc_fail_after = -1;
}

/* Sweep every output capacity through the internal encoder stages so the
** progressive "destination too small" guards (bitstream overflow, FSE table
** serialization, sequence/literal section sizing) all get exercised. */
static void run_encoder_buffer_seeds(void) {
    uzstd__ctx cx;
    uzstd__seq *seqs = (uzstd__seq *)malloc(256 * sizeof *seqs);
    uzstd__u8 *lit = (uzstd__u8 *)malloc(4096);
    uzstd__u8 *buf = (uzstd__u8 *)malloc(8192); /* must hold the largest swept capacity */
    size_t cap;
    int i;
    static const uzstd__u32 nlits[] = {64, 200, 1000, 2048};

    if (!seqs || !lit || !buf)
        die("encoder buffer seed allocation failed");
    UZSTD_MEMSET(&cx, 0, sizeof cx);

    /* varied sequences so all three sequence FSE tables are non-degenerate */
    for (i = 0; i < 256; i++) {
        seqs[i].ll = (uzstd__u32)(i % 24);
        seqs[i].ml = (uzstd__u32)(3 + (i % 30));
        seqs[i].ov = (uzstd__u32)(1 + (i % 50));
    }
    cx.seqs = seqs;
    cx.nseq = 256;
    for (cap = 0; cap <= 1200; cap++)
        (void)uzstd__encode_sequences(&cx, buf, buf + cap);

    /* varied literals to drive the Huffman literal path through every guard */
    for (i = 0; i < 4096; i++)
        lit[i] = (uzstd__u8)((i * 7 + (i >> 3)) & 0x3F);
    cx.lit = lit;
    for (i = 0; i < (int)(sizeof nlits / sizeof nlits[0]); i++) {
        cx.nlit = nlits[i];
        for (cap = 0; cap <= 2400; cap++)
            (void)uzstd__lit_section(&cx, buf, cap);
    }

    /* high-entropy literals: Huffman is not beneficial -> raw fall-through path */
    {
        FuzzRng r; r.s = 0xfeedfaceull;
        for (i = 0; i < 4096; i++) lit[i] = (uzstd__u8)(rng_u32(&r) >> 24);
        cx.nlit = 4096;
        for (cap = 0; cap <= 4200; cap += 7)
            (void)uzstd__lit_section(&cx, buf, cap);
        /* high-entropy 4-stream literals: sub-stream failures at tight capacities */
        for (i = 0; i < 4096; i++) lit[i] = (uzstd__u8)('a' + (rng_u32(&r) % 40u));
        cx.nlit = 2048;
        for (cap = 120; cap <= 1200; cap++)
            (void)uzstd__lit_section(&cx, buf, cap);
    }

    /* small (<64) multi-symbol literal counts: Huffman gate skipped -> raw */
    {
        static const uzstd__u32 smalls[] = {2, 8, 30, 63};
        int j;
        for (i = 0; i < 4096; i++) lit[i] = (uzstd__u8)("xyz!"[i & 3]);
        for (j = 0; j < (int)(sizeof smalls / sizeof smalls[0]); j++) {
            cx.nlit = smalls[j];
            for (cap = 0; cap <= 100; cap++)
                (void)uzstd__lit_section(&cx, buf, cap);
        }
    }

    /* direct calls into the Huffman tree-description writer to reach its tiny-cap
    ** guard and the "FSE weights not beneficial -> direct nibbles" decisions */
    {
        uzstd__u8 hlen[256];
        uzstd__u32 cnt2[256];
        int ls = 0, mb, s;
        UZSTD_MEMSET(cnt2, 0, sizeof cnt2);
        cnt2[0] = 10; cnt2[1] = 3; cnt2[2] = 1;            /* few symbols -> direct wins */
        mb = uzstd__huf_lengths(cnt2, &ls, hlen);
        for (cap = 0; cap <= 300; cap++)
            (void)uzstd__huf_tree(buf, cap, hlen, ls, mb);
        /* all 256 symbols with high-entropy weights: the FSE-coded weights blow
        ** the 127-byte budget, so FSE is rejected and the direct path with >128
        ** symbols is taken (and rejected) */
        UZSTD_MEMSET(cnt2, 0, sizeof cnt2);
        for (s = 0; s < 256; s++) cnt2[s] = 1u << (s % 12);
        mb = uzstd__huf_lengths(cnt2, &ls, hlen);
        for (cap = 130; cap <= 600; cap++)
            (void)uzstd__huf_tree(buf, cap, hlen, ls, mb);
    }

    /* >128 distinct symbols force the direct (nibble) tree-description path,
    ** including the nw>128 rejection (huf_tree returns 0). */
    {
        uzstd__u8 hlen[256];
        uzstd__u32 cnt[256];
        int last_sym = 0, maxbits, s;
        UZSTD_MEMSET(cnt, 0, sizeof cnt);
        for (s = 0; s < 200; s++) cnt[s] = (uzstd__u32)(1 + (s & 3));
        maxbits = uzstd__huf_lengths(cnt, &last_sym, hlen);
        (void)uzstd__huf_tree(buf, 4096, hlen, last_sym, maxbits);
        /* single-symbol histogram: fewer than 2 symbols -> early return */
        UZSTD_MEMSET(cnt, 0, sizeof cnt);
        cnt[7] = 100;
        (void)uzstd__huf_lengths(cnt, &last_sym, hlen);
    }

    /* public compress rejects a destination smaller than the bound */
    {
        unsigned char small_in[64], small_out[8];
        UZSTD_MEMSET(small_in, 'k', sizeof small_in);
        if (!UZSTD_IS_ERROR(uzstd_compress(small_out, sizeof small_out,
                                           small_in, sizeof small_in, 5)))
            die("compress accepted an undersized destination");
    }

    free(buf);
    free(lit);
    free(seqs);
}

/* Feed valid frames of several literal/sequence flavors, then sweep every
** truncation length and every too-small output capacity, so the decoder's
** bounds and malformed-input guards are all hit. */
static void run_decoder_buffer_seeds(void) {
    unsigned char *in = (unsigned char *)malloc(8192);
    unsigned char *comp = (unsigned char *)malloc(uzstd_compress_bound(8192));
    unsigned char *out = (unsigned char *)malloc(8192);
    size_t p, t, cap, csz, n;
    int flavor, level;

    if (!in || !comp || !out)
        die("decoder buffer seed allocation failed");

    for (flavor = 0; flavor < 6; flavor++) {
        n = (flavor == 5) ? 37 : 6000;
        switch (flavor) {
        case 0: memset(in, 'Q', n); break;                       /* RLE block/literals */
        case 1: for (p = 0; p < n; p++) in[p] = (unsigned char)"the quick brown fox "[p % 20]; break;
        case 2: for (p = 0; p < n; p++) in[p] = (unsigned char)(p & 0xFF); break; /* huffman 4-stream */
        case 3: for (p = 0; p < n; p++) in[p] = (unsigned char)((p * 31u + (p >> 2)) & 0xFF); break;
        case 4: for (p = 0; p < n; p++) in[p] = (unsigned char)('a' + (p % 3)); break;
        default: for (p = 0; p < n; p++) in[p] = (unsigned char)"hi"[p & 1]; break;
        }
        for (level = 1; level <= 9; level += 4) {
            csz = uzstd_compress(comp, uzstd_compress_bound(n), in, n, level);
            if (UZSTD_IS_ERROR(csz))
                die("decoder buffer seed compression failed");
            for (t = 0; t <= csz; t++) {
                (void)uzstd_frame_content_size(comp, t);
                (void)uzstd_decompress(out, 8192, comp, t);       /* truncation sweep */
            }
            for (cap = 0; cap <= n && cap <= 8192; cap++)
                (void)uzstd_decompress(out, cap, comp, csz);       /* output-overflow sweep */
        }
    }

    free(out);
    free(comp);
    free(in);
}

/* Drive the decoder's internal malformed-input guards directly. These hit the
** "claimed size exceeds what is present" paths that frame truncation can't reach
** (truncation fails at the outer block-size check before the block is parsed). */
static void run_decoder_negative_seeds(void) {
    uzstd__dctx *dcx = (uzstd__dctx *)malloc(sizeof *dcx);
    uzstd__u8 b[64], out[512], padded[64];
    uzstd__u8 *blk = padded + 8;
    const uzstd__u8 *litp;
    size_t litsz;
    int log;
    short norm[64];
    uzstd__dte dt[1 << 9];

    if (!dcx)
        die("decoder negative seed allocation failed");
    UZSTD_MEMSET(dcx, 0, sizeof *dcx);
    UZSTD_MEMSET(padded, 0, sizeof padded);

    /* read_ncount: truncated / over-long table descriptions (772, 787) */
    UZSTD_MEMSET(b, 0, sizeof b);
    UZSTD_MEMSET(norm, 0, sizeof norm);
    (void)uzstd__read_ncount(norm, 35, 9, &log, b, 1);
    UZSTD_MEMSET(b, 0xFF, sizeof b);
    b[0] = 0;
    UZSTD_MEMSET(norm, 0, sizeof norm);
    (void)uzstd__read_ncount(norm, 35, 9, &log, b, sizeof b);

    /* huf_tree_d: no input, FSE weights longer than avail, all-zero weights (815,830,847) */
    UZSTD_MEMSET(b, 0, sizeof b);
    (void)uzstd__huf_tree_d(dcx, b, 0);
    b[0] = 60;
    (void)uzstd__huf_tree_d(dcx, b, 3);
    b[0] = 129; b[1] = 0x00;
    (void)uzstd__huf_tree_d(dcx, b, 2);

    /* lits_d: every size-format with too-small avail (882,885,886,895,904,909,923) */
    (void)uzstd__lits_d(dcx, b, 0, &litp, &litsz);
    b[0] = 0x04; (void)uzstd__lits_d(dcx, b, 1, &litp, &litsz);
    b[0] = 0x0C; (void)uzstd__lits_d(dcx, b, 2, &litp, &litsz);
    b[0] = 0x01; (void)uzstd__lits_d(dcx, b, 1, &litp, &litsz);
    b[0] = 0x02; (void)uzstd__lits_d(dcx, b, 2, &litp, &litsz);   /* comp sf0 avail<3 */
    b[0] = 0x0A; (void)uzstd__lits_d(dcx, b, 3, &litp, &litsz);   /* comp sf2 avail<4 */
    b[0] = 0x0E; (void)uzstd__lits_d(dcx, b, 4, &litp, &litsz);   /* comp sf3 avail<5 */
    b[0] = 0x06; b[1] = 0x00; b[2] = 0x08; /* type2 sf1 (4-stream), regen=0..., csz small */
    (void)uzstd__lits_d(dcx, b, 11, &litp, &litsz);
    dcx->huf_ok = 1; b[0] = 0x03; b[1] = 0x00; b[2] = 0x08; /* treeless w/ prior tree */
    (void)uzstd__lits_d(dcx, b, 11, &litp, &litsz);
    dcx->huf_ok = 0;

    /* seq_prep: RLE with no/oversized code, FSE with bad table (960, 970) */
    (void)uzstd__seq_prep(dcx, 0, 1, b, 0);
    b[0] = 99; (void)uzstd__seq_prep(dcx, 1, 1, b, 1);
    b[0] = 0;  (void)uzstd__seq_prep(dcx, 0, 2, b, 1);

    /* fse_dtable: normalized counts that don't fill the table (748) */
    UZSTD_MEMSET(norm, 0, sizeof norm);
    norm[0] = 1;
    (void)uzstd__fse_dtable(dt, norm, 0, 5);

    /* block_d: truncated sequence-count, then a failing sequence table (989,990,1001) */
    {
        uzstd__u8 *op = out;
        blk[0] = 0x00; blk[1] = 0x80;
        op = out; (void)uzstd__block_d(dcx, &op, out + sizeof out, out, blk, 2);
        blk[1] = 0xFF;
        op = out; (void)uzstd__block_d(dcx, &op, out + sizeof out, out, blk, 2);
        blk[1] = 1; blk[2] = (uzstd__u8)(2u << 6); /* 1 seq, ll table FSE-mode but no table bytes */
        op = out; (void)uzstd__block_d(dcx, &op, out + sizeof out, out, blk, 3);
    }

    /* frame_content_size: header (with 1-byte dict id) consumes all bytes, so the
    ** promised 1-byte content size is absent -> case-0 truncation return */
    UZSTD_MEMSET(b, 0, sizeof b);
    uzstd__wle32(b, 0xFD2FB528u);
    b[4] = 0x21; /* fcsf=0, single-segment, 1-byte dict id */
    (void)uzstd_frame_content_size(b, 6);

    /* decompress: skippable < 8, window descriptor absent, dict id absent (1081,1096,1100) */
    uzstd__wle32(b, 0x184D2A50u);
    (void)uzstd_decompress(out, sizeof out, b, 5);
    uzstd__wle32(b, 0xFD2FB528u);
    b[4] = 0x00; /* not single-segment: window-descriptor byte absent (1096) */
    (void)uzstd_decompress(out, sizeof out, b, 5);
    b[4] = 0x21; /* single-segment + 1-byte dict id, which is absent (1100/1101) */
    (void)uzstd_decompress(out, sizeof out, b, 5);

    free(dcx);
}

/* Multi-frame input so the decoder reuses its already-allocated context (1114). */
static void run_multiframe_seed(void) {
    unsigned char in[256], comp[1024], out[600];
    size_t c1, c2;
    int i;
    for (i = 0; i < 256; i++) in[i] = (unsigned char)("abc"[i % 3]);
    c1 = uzstd_compress(comp, sizeof comp, in, 200, 5);
    if (UZSTD_IS_ERROR(c1)) die("multiframe seed compress 1 failed");
    c2 = uzstd_compress(comp + c1, sizeof comp - c1, in + 50, 150, 3);
    if (UZSTD_IS_ERROR(c2)) die("multiframe seed compress 2 failed");
    if (uzstd_decompress(out, sizeof out, comp, c1 + c2) != 350)
        die("multiframe seed roundtrip failed");
}

/* Exhaustively replace every byte of small valid frames with all 256 values and
** feed the result to the decoder. This deterministically drives the decoder's
** internal malformed-input guards (which whole-frame truncation cannot reach,
** since truncation fails at the outer block-size check first). */
static void run_exhaustive_mutation_seed(void) {
    size_t cap_in = 6000;
    unsigned char *in = (unsigned char *)malloc(cap_in);
    unsigned char *comp = (unsigned char *)malloc(uzstd_compress_bound(cap_in));
    unsigned char *work = (unsigned char *)malloc(uzstd_compress_bound(cap_in));
    unsigned char *out = (unsigned char *)malloc(cap_in);
    FuzzRng r;
    size_t csz, n, p;
    int flavor, level;
    unsigned val;

    if (!in || !comp || !work || !out)
        die("exhaustive mutation seed allocation failed");
    r.s = 0x51515151c0ffeeull;

    /* flavors 0-4 are small (full 256-value sweep is cheap); flavor 5 is large
    ** and entropy-rich so it carries FSE sequence tables and 4-stream Huffman
    ** literals, exposing the decoder's table-parsing guards to mutation. */
    for (flavor = 0; flavor < 6; flavor++) {
        n = (flavor < 5) ? 300 : cap_in;
        switch (flavor) {
        case 0: memset(in, 'M', n); break;
        case 1: for (p = 0; p < n; p++) in[p] = (unsigned char)"abcdefgh"[p & 7]; break;
        case 2: for (p = 0; p < n; p++) in[p] = (unsigned char)(p & 0xFF); break;
        case 3: for (p = 0; p < n; p++) in[p] = (unsigned char)((p * 37u) & 0xFF); break;
        case 4: for (p = 0; p < n; p++) in[p] = (unsigned char)('a' + (p % 5)); break;
        default:
            for (p = 0; p < n; p++) {
                if ((rng_u32(&r) & 3u) && p >= 8)
                    in[p] = in[p - 1 - (rng_u32(&r) % 8u)]; /* short matches -> sequences */
                else
                    in[p] = (unsigned char)('a' + (rng_u32(&r) % 45u)); /* 45 syms -> huffman */
            }
            break;
        }
        for (level = 1; level <= 9; level += 2) {
            csz = uzstd_compress(comp, uzstd_compress_bound(n), in, n, level);
            if (UZSTD_IS_ERROR(csz))
                continue;
            for (p = 0; p < csz; p++) {
                memcpy(work, comp, csz);
                for (val = 0; val < 256; val++) {
                    work[p] = (unsigned char)val;
                    (void)uzstd_frame_content_size(work, csz);
                    (void)uzstd_decompress(out, cap_in, work, csz);
                }
            }
            if (flavor == 5)
                break; /* one level is enough for the expensive large sweep */
        }
    }

    free(out);
    free(work);
    free(comp);
    free(in);
}

/* Build valid FSE table descriptions with the encoder, then (a) sweep the write
** capacity to hit the serializer's overflow guards and (b) exhaustively corrupt
** the bytes and feed them to the decoder's table parsers, hitting the malformed-
** table rejection paths that random whole-frame fuzzing reaches only rarely. */
static void run_fse_table_seeds(void) {
    uzstd__dctx *dcx = (uzstd__dctx *)malloc(sizeof *dcx);
    uzstd__u32 cnt[64];
    short norm[64];
    uzstd__u8 tbl[160], work[256]; /* >= avail + 8 so the bit reader's look-ahead stays in bounds */
    short onorm[64];
    int olog, s, variant;
    size_t sz, cap, pos;
    unsigned val;
    /* (max_sym, accuracy log, gappy?) chosen to mirror ll/of/ml and a zero-run case */
    static const int msym[] = {35, 28, 52, 15};
    static const int mlog[] = {9, 8, 9, 6};

    if (!dcx) die("fse table seed allocation failed");
    UZSTD_MEMSET(dcx, 0, sizeof *dcx);

    for (variant = 0; variant < 4; variant++) {
        int ms = msym[variant], lg = mlog[variant] > 6 ? 6 : mlog[variant];
        uzstd__u32 total = 0;
        UZSTD_MEMSET(cnt, 0, sizeof cnt);
        for (s = 0; s <= ms; s++) {
            /* leave wide gaps so the description carries zero-run flags */
            uzstd__u32 c = (s % 5 == 0) ? (uzstd__u32)(1 + (s * 7 % 23)) : 0;
            cnt[s] = c; total += c;
        }
        if (total < 2) { cnt[0] += 2; total += 2; }
        uzstd__fse_norm(cnt, ms, total, lg, norm);
        sz = uzstd__fse_write_ncount(tbl, sizeof tbl, norm, lg);
        if (!sz) continue;

        /* sweep serialization capacity: trips the "destination too small" guards
        ** including the zero-run-flag emission paths */
        for (cap = 0; cap <= sz + 2; cap++)
            (void)uzstd__fse_write_ncount(work, cap, norm, lg);

        /* exhaustive single-byte corruption fed to the matching decoder parsers */
        for (pos = 0; pos < sz; pos++) {
            UZSTD_MEMSET(work + sz, 0, sizeof work - sz);
            for (val = 0; val < 256; val++) {
                UZSTD_MEMCPY(work, tbl, sz);
                work[pos] = (uzstd__u8)val;
                UZSTD_MEMSET(onorm, 0, sizeof onorm);
                (void)uzstd__read_ncount(onorm, ms, mlog[variant], &olog, work, sz);
                (void)uzstd__read_ncount(onorm, ms, mlog[variant], &olog, work, pos + 1);
                /* generous avail: corrupted counts run on into the trailing zeros,
                ** reaching the over-read / remaining<1 / runaway-symbol guards */
                (void)uzstd__read_ncount(onorm, ms, mlog[variant], &olog, work, sz + 40);
                (void)uzstd__read_ncount(onorm, 5, mlog[variant], &olog, work, sz + 40);
                if (variant < 3)
                    (void)uzstd__seq_prep(dcx, variant, 2, work, sz);
                else {
                    uzstd__u8 wt[177];
                    wt[0] = (uzstd__u8)sz;       /* huf weights: length prefix then table */
                    UZSTD_MEMCPY(wt + 1, work, sz);
                    (void)uzstd__huf_tree_d(dcx, wt, sz + 1);
                }
            }
        }
    }
    free(dcx);
}

/* A valid normalized distribution carrying a "<1 probability" (-1) symbol, plus a
** long interior zero-run. fse_norm never emits -1, so this drives the serializer's
** negative-count and >=24 zero-run paths (and, round-tripped, the reader's). */
static void run_neg_prob_seed(void) {
    short norm[64];
    uzstd__u8 tbl[208];
    short onorm[64];
    int olog;
    size_t sz, cap;
    /* log 5 -> table size 32; counts sum to 32 counting |-1| as 1 */
    UZSTD_MEMSET(norm, 0, sizeof norm);
    norm[0] = 14; norm[1] = -1; norm[2] = 1; norm[40] = 16; /* 37 zero syms between 2 and 40 */
    sz = uzstd__fse_write_ncount(tbl, sizeof tbl, norm, 5);
    if (!sz) return;
    for (cap = 0; cap <= sz + 2; cap++)            /* serializer overflow incl. zero-run path */
        (void)uzstd__fse_write_ncount(tbl, cap, norm, 5);
    sz = uzstd__fse_write_ncount(tbl, sizeof tbl, norm, 5);
    UZSTD_MEMSET(tbl + sz, 0, sizeof tbl - sz);
    UZSTD_MEMSET(onorm, 0, sizeof onorm);
    (void)uzstd__read_ncount(onorm, 40, 9, &olog, tbl, sz);  /* round-trips the -1 + zero-run */
}

/* Directly fuzz the decoder's internal parsers with random bytes (fixed seed, so
** it is deterministic). Random table/section/block bytes reach the semantic
** rejection guards that structured frame fuzzing hits only rarely, far more
** reliably than corrupting otherwise-valid tables. All buffers are padded so the
** backward bit reader's 8-byte look-ahead stays in bounds. */
static void run_internal_parser_fuzz(void) {
    uzstd__dctx *dcx = (uzstd__dctx *)malloc(sizeof *dcx);
    uzstd__u8 buf[80], padded[96], out[640];
    short onorm[64];
    int olog, it;
    const uzstd__u8 *lp;
    size_t lsz;
    FuzzRng r;

    if (!dcx) die("internal parser fuzz allocation failed");
    UZSTD_MEMSET(dcx, 0, sizeof *dcx);
    r.s = 0x0abcdef123456789ull;

    for (it = 0; it < 400000; it++) {
        size_t len = 1 + rng_range(&r, 48);
        size_t avail = 1 + rng_range(&r, len);
        size_t i;
        uzstd__u8 *op = out;
        for (i = 0; i < sizeof buf; i++) buf[i] = (uzstd__u8)(rng_u32(&r) >> 24);

        UZSTD_MEMSET(onorm, 0, sizeof onorm);
        (void)uzstd__read_ncount(onorm, 35, 9, &olog, buf, avail);
        UZSTD_MEMSET(onorm, 0, sizeof onorm);
        (void)uzstd__read_ncount(onorm, 52, 9, &olog, buf, avail);
        UZSTD_MEMSET(onorm, 0, sizeof onorm);
        (void)uzstd__read_ncount(onorm, 15, 6, &olog, buf, avail);
        (void)uzstd__seq_prep(dcx, it % 3, 2, buf, avail);
        dcx->huf_ok = it & 1;                 /* exercise treeless-with/without prior tree */
        (void)uzstd__huf_tree_d(dcx, buf, avail);
        (void)uzstd__lits_d(dcx, buf, avail, &lp, &lsz);
        (void)uzstd_frame_content_size(buf, avail);

        UZSTD_MEMSET(padded, 0, 8);
        UZSTD_MEMCPY(padded + 8, buf, len);
        UZSTD_MEMSET(padded + 8 + len, 0, 8);
        (void)uzstd__block_d(dcx, &op, out + sizeof out, out, padded + 8, len);
    }
    free(dcx);
}

static void run_coverage_seeds(void) {
    /* Fast correctness assertions and targeted malformed-input checks: always run. */
    run_malformed_frame_seeds();
    run_internal_coverage_seeds();
    run_alloc_fail_seeds();
    run_neg_prob_seed();
    run_decoder_negative_seeds();
    run_multiframe_seed();
    /* Heavy coverage-maximizing sweeps (seconds to tens of seconds): opt-in, so a
    ** plain fuzz run (and each forever-restart) starts quickly. tools/coverage.sh
    ** sets UZSTD_FUZZ_COVERAGE=1 to drive the branch-coverage gate. */
    if (env_ull("UZSTD_FUZZ_COVERAGE", 0)) {
        run_fse_table_seeds();
        run_internal_parser_fuzz();
        run_encoder_buffer_seeds();
        run_decoder_buffer_seeds();
        run_exhaustive_mutation_seed();
    }
}

static void check_roundtrip(const FuzzConfig *cfg, const FuzzCase *fc,
                            const unsigned char *src, unsigned char *compressed,
                            unsigned char *decoded, unsigned char *scratch) {
    size_t cap = uzstd_compress_bound(fc->size);
    size_t csz, dsz, bad_cap;
    unsigned long long fcs;

    if (cap < fc->size)
        die("compress bound overflow at size %llu", (unsigned long long)fc->size);

    csz = uzstd_compress(compressed, cap, src, fc->size, fc->level);
    if (UZSTD_IS_ERROR(csz)) {
        save_named_case(cfg, "failure_compress", 0, src, fc->size, fc);
        die("compress failed: iter=%llu size=%llu pattern=%d level=%d",
            fc->iter, (unsigned long long)fc->size, fc->pattern, fc->level);
    }

    fcs = uzstd_frame_content_size(compressed, csz);
    if (fcs != (unsigned long long)fc->size) {
        save_named_case(cfg, "failure_fcs", 0, src, fc->size, fc);
        die("frame content size mismatch: got=%llu want=%llu",
            fcs, (unsigned long long)fc->size);
    }

    bad_cap = fc->size ? fc->size - 1 : 0;
    if (fc->size && !UZSTD_IS_ERROR(uzstd_decompress(decoded, bad_cap, compressed, csz))) {
        save_named_case(cfg, "failure_small_dst", 0, src, fc->size, fc);
        die("decompress unexpectedly accepted too-small output buffer");
    }

    dsz = uzstd_decompress(decoded, fc->size ? fc->size : 1, compressed, csz);
    if (UZSTD_IS_ERROR(dsz) || dsz != fc->size ||
        (fc->size && memcmp(src, decoded, fc->size) != 0)) {
        save_named_case(cfg, "failure_roundtrip", 0, src, fc->size, fc);
        die("roundtrip mismatch: iter=%llu csz=%llu dsz=%llu size=%llu",
            fc->iter, (unsigned long long)csz, (unsigned long long)dsz,
            (unsigned long long)fc->size);
    }

    if (csz) {
        FuzzRng corrupt_rng;
        size_t i, trunc;
        corrupt_rng.s = fc->seed ^ 0xd1b54a32d192ed03ull;
        trunc = rng_range(&corrupt_rng, csz);
        (void)uzstd_decompress(scratch, fc->size + 131072u, compressed, trunc);
        memcpy(scratch, compressed, csz);
        mutate_bytes(&corrupt_rng, scratch, csz);
        for (i = 0; i < 3; i++) {
            (void)uzstd_frame_content_size(scratch, csz);
            (void)uzstd_decompress(decoded, fc->size ? fc->size : 1, scratch, csz);
            mutate_bytes(&corrupt_rng, scratch, csz);
        }
    }
}

static int worker_loop(const FuzzConfig *cfg, unsigned restart_index) {
    FuzzRng rng;
    unsigned char *src, *compressed, *decoded, *scratch;
    unsigned long long iter = 0;
    size_t out_cap = uzstd_compress_bound(cfg->max_size);
    size_t scratch_cap = out_cap + cfg->max_size + 131072u + 64u;

    rng.s = cfg->seed ^ (0x9e3779b97f4a7c15ull * (uint64_t)(restart_index + 1u));
    if (!rng.s)
        rng.s = 1;

    src = (unsigned char *)malloc(cfg->max_size ? cfg->max_size : 1);
    compressed = (unsigned char *)malloc(out_cap ? out_cap : 1);
    decoded = (unsigned char *)malloc(cfg->max_size ? cfg->max_size : 1);
    scratch = (unsigned char *)malloc(scratch_cap ? scratch_cap : 1);
    if (!src || !compressed || !decoded || !scratch)
        die("allocation failed");

    run_coverage_seeds();

    for (;;) {
        FuzzCase fc;
        fc.iter = iter;
        fc.seed = rng.s;
        fc.size = choose_size(&rng, cfg->max_size);
        fc.pattern = (int)(rng_u32(&rng) % 10u);
        fc.level = (int)(rng_u32(&rng) % 12u) - 1;

        fill_case(&rng, src, fc.size, fc.pattern);
        write_current_case(cfg, &fc, src);
        check_roundtrip(cfg, &fc, src, compressed, decoded, scratch);

        iter++;
        if (cfg->log_every && iter % cfg->log_every == 0) {
            fprintf(stderr, "fuzz iter=%llu restart=%u seed=%llu\n",
                    iter, restart_index, (unsigned long long)rng.s);
            fflush(stderr);
        }
        if (cfg->max_iters && iter >= cfg->max_iters)
            break;
    }

    free(scratch);
    free(decoded);
    free(compressed);
    free(src);
    return 0;
}

static void set_core_limit(int enabled) {
    struct rlimit rl;
    rl.rlim_cur = enabled ? RLIM_INFINITY : 0;
    rl.rlim_max = enabled ? RLIM_INFINITY : 0;
    (void)setrlimit(RLIMIT_CORE, &rl);
}

static void load_config(FuzzConfig *cfg) {
    const char *dir = getenv("UZSTD_FUZZ_DIR");
    char cwd[PATH_MAX];
    unsigned long long seed;
    if (!dir || !dir[0])
        dir = "uzstd_fuzz_artifacts";
    if (dir[0] == '/') {
        snprintf(cfg->dir, sizeof cfg->dir, "%s", dir);
    } else {
        if (!getcwd(cwd, sizeof cwd))
            die("getcwd failed");
        snprintf(cfg->dir, sizeof cfg->dir, "%s/%s", cwd, dir);
    }

    cfg->max_crashes = (unsigned)env_ull("UZSTD_FUZZ_MAX_CRASHES", 10);
    cfg->max_iters = env_ull("UZSTD_FUZZ_MAX_ITERS", 0);
    cfg->log_every = env_ull("UZSTD_FUZZ_LOG_EVERY", 10000);
    cfg->max_size = (size_t)env_ull("UZSTD_FUZZ_MAX_SIZE", 524288);
    seed = env_ull("UZSTD_FUZZ_SEED", 0);
    cfg->seed = seed ? (uint64_t)seed :
        ((uint64_t)time(0) << 32) ^ (uint64_t)getpid() ^ 0xa5a5a5a55a5a5a5aull;
    if (cfg->max_size < 1)
        cfg->max_size = 1;
}

int main(void) {
    FuzzConfig cfg;
    unsigned crashes = 0, restarts = 0;

    load_config(&cfg);
    mkdir_p_one(cfg.dir);

    fprintf(stderr,
            "uzstd fuzz: dir=%s seed=%llu max_size=%llu max_crashes=%u max_iters=%llu\n",
            cfg.dir, (unsigned long long)cfg.seed,
            (unsigned long long)cfg.max_size, cfg.max_crashes,
            cfg.max_iters);

    for (;;) {
        pid_t pid = fork();
        int status;
        if (pid < 0)
            die("fork failed: %s", strerror(errno));
        if (pid == 0) {
            set_core_limit(crashes < cfg.max_crashes);
            if (chdir(cfg.dir) != 0)
                die("chdir %s: %s", cfg.dir, strerror(errno));
            return worker_loop(&cfg, restarts);
        }
        if (waitpid(pid, &status, 0) < 0)
            die("waitpid failed: %s", strerror(errno));
        if (WIFEXITED(status)) {
            int code = WEXITSTATUS(status);
            if (code == 0) {
                fprintf(stderr, "worker exited cleanly\n");
                return 0;
            }
            fprintf(stderr, "worker stopped with exit code %d\n", code);
            return code;
        }
        if (WIFSIGNALED(status)) {
            char current[PATH_MAX], current_meta[PATH_MAX];
            char crash_bin[PATH_MAX], crash_txt[PATH_MAX];
            char name[64];
            int sig = WTERMSIG(status);
            crashes++;
            snprintf(name, sizeof name, "crash_%02u.bin", crashes);
            join_path(crash_bin, sizeof crash_bin, cfg.dir, name);
            snprintf(name, sizeof name, "crash_%02u.txt", crashes);
            join_path(crash_txt, sizeof crash_txt, cfg.dir, name);
            join_path(current, sizeof current, cfg.dir, "current.bin");
            join_path(current_meta, sizeof current_meta, cfg.dir, "current.txt");
            copy_file_if_present(current, crash_bin);
            copy_file_if_present(current_meta, crash_txt);
            fprintf(stderr, "worker crashed with signal %d; saved crash_%02u.*\n",
                    sig, crashes);
            if (crashes >= cfg.max_crashes) {
                fprintf(stderr, "crash limit reached; stopping permanently\n");
                return 128 + sig;
            }
            restarts++;
            sleep(1);
            continue;
        }
        fprintf(stderr, "worker ended unexpectedly; stopping\n");
        return 3;
    }
}
