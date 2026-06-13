/* uzstd.c - command-line test, interoperability, and benchmark helper. */
#define UZSTD_IMPLEMENTATION
#include "uzstd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


static uzstd__u32 uzstd__test_rng = 0x12345678;
static uzstd__u32 uzstd__test_rand(void) {
    uzstd__test_rng = uzstd__test_rng*1664525u + 1013904223u;
    return uzstd__test_rng >> 24;
}

static const char *uzstd__tmpdir(void) {
    const char *dir = getenv("UZSTD_TMPDIR");
    if (dir && dir[0]) return dir;
#ifdef _WIN32
    return ".";
#else
    return "/tmp";
#endif
}

static int uzstd__tmp_path(char *path, size_t path_cap, const char *name) {
    int n = snprintf(path, path_cap, "%s/%s", uzstd__tmpdir(), name);
    return n > 0 && (size_t)n < path_cap;
}

static int uzstd__test_one(const char *name, const uzstd__u8 *data, size_t n, int level) {
    size_t cap = uzstd_compress_bound(n), csize, dsize;
    uzstd__u8 *out = (uzstd__u8*)malloc(cap);
    uzstd__u8 *dec = (uzstd__u8*)malloc(n ? n : 1);
    char path[512], tmpname[256];
    FILE *f;
    int ok;
    csize = uzstd_compress(out, cap, data, n, level);
    dsize = UZSTD_IS_ERROR(csize) ? (size_t)-1 : uzstd_decompress(dec, n, out, csize);
    ok = !UZSTD_IS_ERROR(csize) && !UZSTD_IS_ERROR(dsize) && dsize == n && (!n || !memcmp(dec, data, n));
    printf("%-12s %9u -> %9u (%.3fx) roundtrip %s\n", name, (unsigned)n, (unsigned)csize,
           !UZSTD_IS_ERROR(csize) && csize ? (double)n/(double)csize : 0.0, ok ? "ok" : "FAIL");
    if (!UZSTD_IS_ERROR(csize)) {
        snprintf(tmpname, sizeof tmpname, "uzstd_%s.zst", name);
        if (uzstd__tmp_path(path, sizeof path, tmpname)) {
            f = fopen(path, "wb");
            if (f) { fwrite(out, 1, csize, f); fclose(f); }
        }
    }
    snprintf(tmpname, sizeof tmpname, "uzstd_%s.bin", name);
    if (uzstd__tmp_path(path, sizeof path, tmpname)) {
        f = fopen(path, "wb");
        if (f) { fwrite(data, 1, n, f); fclose(f); }
    }
    free(out); free(dec);
    return ok;
}

int main(int argc, char **argv) {
    if (argc > 2 && !strcmp(argv[1], "-d")) { /* uzstd -d <file.zst>: decompress to $UZSTD_TMPDIR/uzstd_out.bin */
        FILE *f = fopen(argv[2], "rb");
        size_t n, cap, dsize;
        unsigned long long fcs;
        uzstd__u8 *buf, *out;
        char path[512];
        if (!f) { perror(argv[2]); return 1; }
        fseek(f, 0, SEEK_END); n = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
        buf = (uzstd__u8*)malloc(n ? n : 1);
        if (n && fread(buf, 1, n, f) != n) { perror("fread"); return 1; }
        fclose(f);
        fcs = uzstd_frame_content_size(buf, n); /* first frame only; pad for multi-frame input */
        cap = n * 64 + (1u<<24);
        if (fcs != (unsigned long long)-1 && (size_t)fcs + (size_t)(fcs >> 2) + 4096 > cap)
            cap = (size_t)fcs + (size_t)(fcs >> 2) + 4096;
        out = (uzstd__u8*)malloc(cap);
        dsize = uzstd_decompress(out, cap, buf, n);
        if (UZSTD_IS_ERROR(dsize)) { printf("decode error\n"); return 1; }
        printf("%u -> %u\n", (unsigned)n, (unsigned)dsize);
        if (!uzstd__tmp_path(path, sizeof path, "uzstd_out.bin")) return 1;
        f = fopen(path, "wb");
        if (!f) { perror(path); return 1; }
        fwrite(out, 1, dsize, f);
        fclose(f);
        return 0;
    }
    if (argc > 1) { /* uzstd <file> [level]: compress to $UZSTD_TMPDIR/uzstd_file.zst */
        FILE *f = fopen(argv[1], "rb");
        size_t n, csize, cap, dsize;
        uzstd__u8 *buf, *out, *dec;
        char path[512];
        if (!f) { perror(argv[1]); return 1; }
        fseek(f, 0, SEEK_END); n = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
        buf = (uzstd__u8*)malloc(n ? n : 1);
        if (n && fread(buf, 1, n, f) != n) { perror("fread"); return 1; }
        fclose(f);
        cap = uzstd_compress_bound(n);
        out = (uzstd__u8*)malloc(cap);
        dec = (uzstd__u8*)malloc(n ? n : 1);
        csize = uzstd_compress(out, cap, buf, n, argc > 2 ? atoi(argv[2]) : 5);
        dsize = UZSTD_IS_ERROR(csize) ? (size_t)-1 : uzstd_decompress(dec, n, out, csize);
        printf("%u -> %u (%.3fx) roundtrip %s\n", (unsigned)n, (unsigned)csize,
               !UZSTD_IS_ERROR(csize) && csize ? (double)n/(double)csize : 0.0,
               (!UZSTD_IS_ERROR(csize) && !UZSTD_IS_ERROR(dsize) && dsize == n && (!n || !memcmp(dec, buf, n))) ? "ok" : "FAIL");
        if (UZSTD_IS_ERROR(csize)) return 1;
        if (!uzstd__tmp_path(path, sizeof path, "uzstd_file.zst")) return 1;
        f = fopen(path, "wb");
        if (!f) { perror(path); return 1; }
        fwrite(out, 1, csize, f);
        fclose(f);
        return 0;
    }
    {   /* built-in corpus */
        int ok = 1;
        size_t i, n = 1<<20;
        uzstd__u8 *buf = (uzstd__u8*)malloc(n);

        ok &= uzstd__test_one("empty", buf, 0, 5);
        buf[0] = 'x';
        ok &= uzstd__test_one("one", buf, 1, 5);
        memset(buf, 'A', n);
        ok &= uzstd__test_one("zeros", buf, n, 5);
        for (i = 0; i < n; i++) buf[i] = (uzstd__u8)(uzstd__test_rand() ^ (uzstd__test_rand() << 3));
        ok &= uzstd__test_one("random", buf, n, 5);
        for (i = 0; i < n; i++) buf[i] = (uzstd__u8)("the quick brown fox jumps over the lazy dog. "[i % 46]);
        ok &= uzstd__test_one("loop", buf, n, 5);
        for (i = 0; i < n; i++) /* texty: skewed symbols + some structure */
            buf[i] = (uzstd__u8)('a' + (uzstd__test_rand() % ((i & 256) ? 4 : 26)));
        ok &= uzstd__test_one("texty", buf, n, 5);
        for (i = 0; i < n; i += 4) uzstd__wle32(buf+i, (uzstd__u32)(1000000 + i/4 + (uzstd__test_rand()&15)));
        ok &= uzstd__test_one("structs", buf, n, 5);
        ok &= uzstd__test_one("small", (const uzstd__u8*)"hello hello hello hello, world world!", 37, 5);
        free(buf);
        printf(ok ? "all compress calls ok\n" : "FAILURE\n");
        return !ok;
    }
}
