/* uzstd.h - micro zstd (RFC 8878) single-header codec.
**
** Include this file for declarations. In one translation unit, define
** UZSTD_IMPLEMENTATION before including it to compile the codec. Define
** UZSTD_NO_COMPRESSOR or UZSTD_NO_DECOMPRESSOR before inclusion to omit one
** side of the implementation.
**
** Encoder produces standard zstd frames decodable by conforming decoders.
** Decoder handles conformant RFC 8878 frames except dictionaries and legacy
** pre-1.0 formats. Checksums are skipped, not verified. API is all-at-once.
*/
#ifndef UZSTD_H
#define UZSTD_H

#include <stddef.h>

#ifndef UZSTD_LINKAGE
#ifdef __cplusplus
#define UZSTD_LINKAGE extern "C"
#else
#define UZSTD_LINKAGE extern
#endif
#endif

#define UZSTD_IS_ERROR(result) ((result) == (size_t)-1)
#define UZSTD_COMPRESS_BOUND(src_size) ((size_t)(src_size) + (((size_t)(src_size)) >> 17) * 3 + 32)

UZSTD_LINKAGE size_t uzstd_compress_bound(size_t src_size);
UZSTD_LINKAGE size_t uzstd_compress(void *dst, size_t dst_cap, const void *src, size_t src_size, int level);
UZSTD_LINKAGE size_t uzstd_decompress(void *dst, size_t dst_cap, const void *src, size_t src_size);
UZSTD_LINKAGE unsigned long long uzstd_frame_content_size(const void *src, size_t src_size);

#endif /* UZSTD_H */

#if defined(UZSTD_IMPLEMENTATION) && (!defined(UZSTD_NO_COMPRESSOR) || !defined(UZSTD_NO_DECOMPRESSOR))

#ifndef UZSTD_MALLOC
#include <stdlib.h>
#define UZSTD_MALLOC(sz) malloc(sz)
#endif
#ifndef UZSTD_FREE
#include <stdlib.h>
#define UZSTD_FREE(p) free(p)
#endif
#ifndef UZSTD_MEMCPY
#include <string.h>
#define UZSTD_MEMCPY(dst, src, n) memcpy((dst), (src), (n))
#endif
#ifndef UZSTD_MEMSET
#include <string.h>
#define UZSTD_MEMSET(dst, c, n) memset((dst), (c), (n))
#endif
#ifndef UZSTD_MEMCMP
#include <string.h>
#define UZSTD_MEMCMP(a, b, n) memcmp((a), (b), (n))
#endif

#ifndef UZSTD_COMMON_C
#define UZSTD_COMMON_C

typedef unsigned char      uzstd__u8;
typedef unsigned short     uzstd__u16;
typedef unsigned int       uzstd__u32;
typedef unsigned long long uzstd__u64;

static int uzstd__highbit(uzstd__u32 v) { int n=0; while (v>1) { v>>=1; n++; } return n; } /* v>0 */


/* ---------------- sequence codes (RFC 8878 tables) ---------------- */
static const uzstd__u32 uzstd__llbase[36] = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,18,20,22,24,28,
    32,40,48,64,128,256,512,1024,2048,4096,8192,16384,32768,65536};
static const uzstd__u8 uzstd__llbits[36] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,2,2,3,3,4,6,7,8,9,10,11,12,13,14,15,16};
/* match length tables in terms of mlv = match_length - 3 */
static const uzstd__u32 uzstd__mlbase[53] = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,
    26,27,28,29,30,31,32,34,36,38,40,44,48,56,64,80,96,128,256,512,1024,2048,4096,8192,16384,32768,65536};
static const uzstd__u8 uzstd__mlbits[53] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,1,1,1,1,2,2,3,3,4,4,5,7,8,9,10,11,12,13,14,15,16};


#endif /* UZSTD_COMMON_C */

#if !defined(UZSTD_NO_COMPRESSOR)
#ifndef UZSTD_COMPRESSOR_C
#define UZSTD_COMPRESSOR_C

static uzstd__u32 uzstd__read32(const void *p) { uzstd__u32 v; UZSTD_MEMCPY(&v, p, 4); return v; }
static void uzstd__wle16(uzstd__u8 *p, uzstd__u32 v) { p[0]=(uzstd__u8)v; p[1]=(uzstd__u8)(v>>8); }
static void uzstd__wle32(uzstd__u8 *p, uzstd__u32 v) { uzstd__wle16(p,v); uzstd__wle16(p+2,v>>16); }

/* ---------------- backward bitstream writer (FSE/Huffman streams) ---------------- */
typedef struct { uzstd__u64 acc; int cnt; uzstd__u8 *start, *p, *end; } uzstd__bw;

static void uzstd__bw_init(uzstd__bw *b, uzstd__u8 *p, uzstd__u8 *end) {
    b->acc = 0; b->cnt = 0; b->start = b->p = p; b->end = end;
}
static void uzstd__bw_add(uzstd__bw *b, uzstd__u32 v, int nb) { /* nb <= 31 */
    b->acc |= (uzstd__u64)(v & ((1u<<nb)-1)) << b->cnt; b->cnt += nb;
    while (b->cnt >= 8) { if (b->p < b->end) *b->p = (uzstd__u8)b->acc; b->p++; b->acc >>= 8; b->cnt -= 8; }
}
/* terminate with marker bit; returns stream size, 0 on overflow */
static size_t uzstd__bw_close(uzstd__bw *b) {
    uzstd__bw_add(b, 1, 1);
    if (b->cnt) { if (b->p < b->end) *b->p = (uzstd__u8)b->acc; b->p++; }
    return b->p > b->end ? 0 : (size_t)(b->p - b->start);
}

/* ---------------- FSE ---------------- */
typedef struct { uzstd__u16 next[1<<9]; uzstd__u32 dnb[64]; int dfs[64]; int log; } uzstd__fse_ct;
typedef struct { uzstd__u32 v; } uzstd__fse_cs;

/* normalized counts (sum = 1<<log, all >= 1 where cnt > 0); requires (1<<log) >= distinct */
static void uzstd__fse_norm(const uzstd__u32 *cnt, int max_sym, uzstd__u32 total, int log, short *norm) {
    int s, larg = 0, diff; uzstd__u32 sum = 0;
    for (s = 0; s <= max_sym; s++) {
        uzstd__u32 nv = cnt[s] ? (uzstd__u32)(((uzstd__u64)cnt[s] << log) / total) : 0;
        if (cnt[s] && !nv) nv = 1;
        norm[s] = (short)nv; sum += nv;
        if (cnt[s] > cnt[larg]) larg = s;
    }
    diff = (1<<log) - (int)sum;
    if (diff > 0) norm[larg] = (short)(norm[larg] + diff);
    while (diff < 0) { /* shave the biggest */
        int big = larg;
        for (s = 0; s <= max_sym; s++) if (norm[s] > norm[big]) big = s;
        norm[big]--; diff++;
    }
}

/* serialize normalized counts; returns bytes written, 0 on overflow */
static size_t uzstd__fse_write_ncount(uzstd__u8 *out, size_t cap, const short *norm, int log) {
    uzstd__u8 *d = out, *dend = out + cap;
    int tsize = 1<<log, remaining = tsize+1, threshold = tsize, nb_bits = log+1;
    uzstd__u32 bits = (uzstd__u32)(log - 5); int bcnt = 4, sym = 0, prev0 = 0;
    while (remaining > 1) {
        if (prev0) {
            int start = sym;
            while (!norm[sym]) sym++;
            while (sym >= start+24) {
                start += 24; bits += 0xFFFFu << bcnt;
                if (d > dend-2) return 0;
                d[0]=(uzstd__u8)bits; d[1]=(uzstd__u8)(bits>>8); d += 2; bits >>= 16;
            }
            while (sym >= start+3) { start += 3; bits += 3u << bcnt; bcnt += 2; }
            bits += (uzstd__u32)(sym - start) << bcnt; bcnt += 2;
            if (bcnt > 16) {
                if (d > dend-2) return 0;
                d[0]=(uzstd__u8)bits; d[1]=(uzstd__u8)(bits>>8); d += 2; bits >>= 16; bcnt -= 16;
            }
        }
        {   int count = norm[sym++], max = (2*threshold-1) - remaining;
            remaining -= count < 0 ? -count : count;
            count++;
            if (count >= threshold) count += max;
            bits += (uzstd__u32)count << bcnt;
            bcnt += nb_bits;
            bcnt -= (count < max);
            prev0 = (count == 1);
            while (remaining < threshold) { nb_bits--; threshold >>= 1; }
        }
        if (bcnt > 16) {
            if (d > dend-2) return 0;
            d[0]=(uzstd__u8)bits; d[1]=(uzstd__u8)(bits>>8); d += 2; bits >>= 16; bcnt -= 16;
        }
    }
    if (d > dend-2) return 0;
    d[0]=(uzstd__u8)bits; d[1]=(uzstd__u8)(bits>>8);
    return (size_t)(d - out) + (size_t)((bcnt+7)/8);
}

static void uzstd__fse_build_ct(uzstd__fse_ct *ct, const short *norm, int max_sym, int log) {
    int tsize = 1<<log, step = (tsize>>1)+(tsize>>3)+3, mask = tsize-1;
    uzstd__u8 tsym[1<<9]; uzstd__u32 cumul[64];
    int s, total = 0; uzstd__u32 pos = 0, u;
    ct->log = log;
    cumul[0] = 0;
    for (s = 0; s <= max_sym; s++) {
        int k; cumul[s+1] = cumul[s] + (uzstd__u32)norm[s];
        for (k = 0; k < norm[s]; k++) { tsym[pos] = (uzstd__u8)s; pos = (pos + (uzstd__u32)step) & (uzstd__u32)mask; }
    }
    for (u = 0; u < (uzstd__u32)tsize; u++) { s = tsym[u]; ct->next[cumul[s]++] = (uzstd__u16)(tsize + u); }
    for (s = 0; s <= max_sym; s++) {
        if (norm[s] == 0)      { ct->dnb[s] = ((uzstd__u32)(log+1)<<16) - (1u<<log); ct->dfs[s] = 0; }
        else if (norm[s] == 1) { ct->dnb[s] = ((uzstd__u32)log<<16) - (1u<<log); ct->dfs[s] = total - 1; total += 1; }
        else {
            int mb = log - uzstd__highbit((uzstd__u32)norm[s]-1);
            ct->dnb[s] = ((uzstd__u32)mb<<16) - ((uzstd__u32)norm[s]<<mb);
            ct->dfs[s] = total - norm[s]; total += norm[s];
        }
    }
}
static void uzstd__fse_rle_ct(uzstd__fse_ct *ct) { /* degenerate 0-bit table */
    UZSTD_MEMSET(ct, 0, sizeof *ct);
}
static void uzstd__fse_init_state(uzstd__fse_cs *st, const uzstd__fse_ct *ct, int s) {
    uzstd__u32 nb = (ct->dnb[s] + (1u<<15)) >> 16;
    st->v = ct->next[(((nb<<16) - ct->dnb[s]) >> nb) + (uzstd__u32)ct->dfs[s]];
}
static void uzstd__fse_encode(uzstd__bw *b, uzstd__fse_cs *st, const uzstd__fse_ct *ct, int s) {
    uzstd__u32 nb = (st->v + ct->dnb[s]) >> 16;
    uzstd__bw_add(b, st->v, (int)nb);
    st->v = ct->next[(st->v >> nb) + (uzstd__u32)ct->dfs[s]];
}
static void uzstd__fse_flush_state(uzstd__bw *b, const uzstd__fse_cs *st, const uzstd__fse_ct *ct) {
    uzstd__bw_add(b, st->v, ct->log);
}

static int uzstd__llcode(uzstd__u32 v) {
    return v<16 ? (int)v : v<24 ? 16+(int)((v-16)>>1) : v<32 ? 20+(int)((v-24)>>2)
         : v<48 ? 22+(int)((v-32)>>3) : v<64 ? 24 : 19+uzstd__highbit(v);
}
static int uzstd__mlcode(uzstd__u32 m) { /* m = match_length - 3 */
    return m<32 ? (int)m : m<40 ? 32+(int)((m-32)>>1) : m<48 ? 36+(int)((m-40)>>2)
         : m<64 ? 38+(int)((m-48)>>3) : m<96 ? 40+(int)((m-64)>>4) : m<128 ? 42 : 36+uzstd__highbit(m);
}


/* ---------------- compression context ---------------- */
#define UZSTD__BLOCK_MAX  (128*1024)

typedef struct { uzstd__u32 ll, ml, ov; } uzstd__seq; /* lit len, match len, offset value */
typedef struct {
    uzstd__u32 *htab, *chain;
    uzstd__seq *seqs;
    uzstd__u8  *lit, *tmp;
    uzstd__u32 rep[3];
    int nseq; uzstd__u32 nlit;
    int depth, lazy, hash_shift;
    size_t tmp_cap;
} uzstd__ctx;

static uzstd__u32 uzstd__hash(uzstd__u32 v, int shift) { return (v * 2654435761u) >> shift; }

static uzstd__u32 uzstd__mlen(const uzstd__u8 *a, const uzstd__u8 *b, const uzstd__u8 *end) {
    const uzstd__u8 *bs = b; /* count match length of b vs a, b limited by end */
    while (b+4 <= end && uzstd__read32(a) == uzstd__read32(b)) { a += 4; b += 4; }
    while (b < end && *a == *b) { a++; b++; }
    return (uzstd__u32)(b - bs);
}

/* search best match at i (must already be inserted in chain); returns length (0 = none) */
static uzstd__u32 uzstd__find(uzstd__ctx *cx, const uzstd__u8 *base, size_t i, size_t end, uzstd__u32 *odist) {
    uzstd__u32 v = uzstd__read32(base+i);
    uzstd__u32 rep_len = 0, rep_dist = 0, cb = 3, cb_dist = 0, m;
    int r, att = cx->depth;
    for (r = 0; r < 3; r++) {
        uzstd__u32 d = cx->rep[r], x;
        if (d > i) continue;
        x = uzstd__read32(base+i-d) ^ v;
        if (!x) {
            uzstd__u32 L = 4 + uzstd__mlen(base+i-d+4, base+i+4, base+end);
            if (L > rep_len) { rep_len = L; rep_dist = d; }
        } else if (rep_len < 3 && !UZSTD_MEMCMP(base+i-d, base+i, 3)) { rep_len = 3; rep_dist = d; }
    }
    m = cx->chain[i];
    while (m && att-- > 0 && i + cb < end) {
        size_t cand = m - 1;
        if (base[cand+cb] == base[i+cb] && uzstd__read32(base+cand) == v) {
            uzstd__u32 L = 4 + uzstd__mlen(base+cand+4, base+i+4, base+end);
            if (L > cb) { cb = L; cb_dist = (uzstd__u32)(i - cand); if (L >= 128) break; }
        }
        m = cx->chain[cand];
    }
    if (rep_len >= 3 && rep_len + 3 > cb) { *odist = rep_dist; return rep_len; }
    if (cb >= 4) { *odist = cb_dist; return cb; } /* cb>3 only when cb_dist was set (>0) */
    return 0;
}

/* map distance -> offset value + update repcodes */
static uzstd__u32 uzstd__offval(uzstd__ctx *cx, uzstd__u32 d, uzstd__u32 ll) {
    uzstd__u32 *R = cx->rep, ov;
    if (ll) {
        if (d == R[0]) return 1;
        else if (d == R[1]) { ov = 2; R[1]=R[0]; R[0]=d; }
        else if (d == R[2]) { ov = 3; R[2]=R[1]; R[1]=R[0]; R[0]=d; }
        else { ov = d + 3;   R[2]=R[1]; R[1]=R[0]; R[0]=d; }
    } else {
        if (d == R[1])      { ov = 1; R[1]=R[0]; R[0]=d; }
        else if (d == R[2]) { ov = 2; R[2]=R[1]; R[1]=R[0]; R[0]=d; }
        else if (d == R[0]-1) { ov = 3; R[2]=R[1]; R[1]=R[0]; R[0]=d; }
        else { ov = d + 3;   R[2]=R[1]; R[1]=R[0]; R[0]=d; }
    }
    return ov;
}

#define UZSTD__INS(p) do { uzstd__u32 h_ = uzstd__hash(uzstd__read32(base+(p)), cx->hash_shift); \
    cx->chain[p] = cx->htab[h_]; cx->htab[h_] = (uzstd__u32)(p)+1; } while (0)

/* parse block [bstart, bstart+bsize) into cx->seqs / cx->lit */
static void uzstd__parse(uzstd__ctx *cx, const uzstd__u8 *base, size_t bstart, size_t bsize) {
    size_t i = bstart, ins = bstart, anchor = bstart, end = bstart + bsize;
    cx->nseq = 0; cx->nlit = 0;
    while (i + 4 <= end) {
        uzstd__u32 len, dist;
        while (ins <= i) { UZSTD__INS(ins); ins++; }
        len = uzstd__find(cx, base, i, end, &dist);
        if (!len) { i++; continue; }
        while (cx->lazy && len < 64 && i + 5 <= end) { /* lazy: try i+1 */
            uzstd__u32 len2, dist2;
            if (ins <= i+1) { UZSTD__INS(ins); ins++; }
            len2 = uzstd__find(cx, base, i+1, end, &dist2);
            if (len2 > len) { i++; len = len2; dist = dist2; } else break;
        }
        while (i > anchor && i > dist && base[i-1] == base[i-dist-1]) { i--; len++; } /* extend back */
        {   uzstd__u32 ll = (uzstd__u32)(i - anchor);
            uzstd__seq *q = &cx->seqs[cx->nseq++];
            UZSTD_MEMCPY(cx->lit + cx->nlit, base + anchor, ll); cx->nlit += ll;
            q->ll = ll; q->ml = len; q->ov = uzstd__offval(cx, dist, ll);
        }
        i += len; anchor = i;
    }
    UZSTD_MEMCPY(cx->lit + cx->nlit, base + anchor, end - anchor); cx->nlit += (uzstd__u32)(end - anchor);
}

/* ---------------- sequences section ---------------- */
/* write table description for one stream; sets *mode, builds ct; 0 = fail */
static size_t uzstd__seq_table(uzstd__u8 *d, size_t cap, const uzstd__u32 *cnt, int max_sym,
                               uzstd__u32 total, int max_log, int *mode, uzstd__fse_ct *ct) {
    short norm[64];
    int s, distinct = 0, last = 0, log;
    size_t sz;
    for (s = 0; s <= max_sym; s++) if (cnt[s]) { distinct++; last = s; }
    if (cap < 8) return 0;
    if (distinct == 1) { *mode = 1; d[0] = (uzstd__u8)last; uzstd__fse_rle_ct(ct); return 1; }
    log = uzstd__highbit(total-1) - 2; /* total >= 2 here (distinct >= 2); floor applied below */
    s = uzstd__highbit((uzstd__u32)max_sym) + 2;
    if (log < s) log = s;
    if (log < 5) log = 5;
    if (log > max_log) log = max_log;
    uzstd__fse_norm(cnt, max_sym, total, log, norm);
    sz = uzstd__fse_write_ncount(d, cap, norm, log);
    if (!sz) return 0;
    uzstd__fse_build_ct(ct, norm, max_sym, log);
    *mode = 2;
    return sz;
}

static size_t uzstd__encode_sequences(uzstd__ctx *cx, uzstd__u8 *dst, uzstd__u8 *dend) {
    uzstd__u8 *d = dst, *modep;
    int ns = cx->nseq, n, mll = 0, mof = 0, mml = 0, modes[3];
    uzstd__u32 cll[36], cof[32], cml[53];
    uzstd__fse_ct ctll, ctof, ctml;
    uzstd__bw bw;
    size_t sz;
    if (dend - d < 4) return 0;
    if (ns < 128) *d++ = (uzstd__u8)ns;
    else if (ns < 0x7F00) { d[0] = (uzstd__u8)((ns>>8)+0x80); d[1] = (uzstd__u8)ns; d += 2; }
    else { d[0] = 0xFF; uzstd__wle16(d+1, (uzstd__u32)(ns - 0x7F00)); d += 3; }
    if (!ns) return (size_t)(d - dst);
    UZSTD_MEMSET(cll, 0, sizeof cll); UZSTD_MEMSET(cof, 0, sizeof cof); UZSTD_MEMSET(cml, 0, sizeof cml);
    for (n = 0; n < ns; n++) {
        uzstd__seq q = cx->seqs[n];
        int lc = uzstd__llcode(q.ll), oc = uzstd__highbit(q.ov), mc = uzstd__mlcode(q.ml - 3);
        cll[lc]++; cof[oc]++; cml[mc]++;
        if (lc > mll) mll = lc; if (oc > mof) mof = oc; if (mc > mml) mml = mc;
    }
    modep = d++;
    if (!(sz = uzstd__seq_table(d, (size_t)(dend-d), cll, mll, (uzstd__u32)ns, 9, &modes[0], &ctll))) return 0;
    d += sz;
    if (!(sz = uzstd__seq_table(d, (size_t)(dend-d), cof, mof, (uzstd__u32)ns, 8, &modes[1], &ctof))) return 0;
    d += sz;
    if (!(sz = uzstd__seq_table(d, (size_t)(dend-d), cml, mml, (uzstd__u32)ns, 9, &modes[2], &ctml))) return 0;
    d += sz;
    *modep = (uzstd__u8)((modes[0]<<6) | (modes[1]<<4) | (modes[2]<<2));
    {   uzstd__fse_cs sll, sof, sml;
        uzstd__seq q = cx->seqs[ns-1];
        int lc = uzstd__llcode(q.ll), oc = uzstd__highbit(q.ov), mc = uzstd__mlcode(q.ml - 3);
        uzstd__bw_init(&bw, d, dend);
        uzstd__fse_init_state(&sml, &ctml, mc);
        uzstd__fse_init_state(&sof, &ctof, oc);
        uzstd__fse_init_state(&sll, &ctll, lc);
        uzstd__bw_add(&bw, q.ll - uzstd__llbase[lc], uzstd__llbits[lc]);
        uzstd__bw_add(&bw, q.ml - 3 - uzstd__mlbase[mc], uzstd__mlbits[mc]);
        uzstd__bw_add(&bw, q.ov - (1u<<oc), oc);
        for (n = ns-2; n >= 0; n--) {
            q = cx->seqs[n];
            lc = uzstd__llcode(q.ll); oc = uzstd__highbit(q.ov); mc = uzstd__mlcode(q.ml - 3);
            uzstd__fse_encode(&bw, &sof, &ctof, oc);
            uzstd__fse_encode(&bw, &sml, &ctml, mc);
            uzstd__fse_encode(&bw, &sll, &ctll, lc);
            uzstd__bw_add(&bw, q.ll - uzstd__llbase[lc], uzstd__llbits[lc]);
            uzstd__bw_add(&bw, q.ml - 3 - uzstd__mlbase[mc], uzstd__mlbits[mc]);
            uzstd__bw_add(&bw, q.ov - (1u<<oc), oc);
        }
        uzstd__fse_flush_state(&bw, &sml, &ctml);
        uzstd__fse_flush_state(&bw, &sof, &ctof);
        uzstd__fse_flush_state(&bw, &sll, &ctll);
        if (!(sz = uzstd__bw_close(&bw))) return 0;
        d += sz;
    }
    return (size_t)(d - dst);
}

/* ---------------- Huffman ---------------- */
/* code lengths (<= 11, kraft-complete) via sorted two-queue; returns maxbits, 0 if < 2 symbols */
static int uzstd__huf_lengths(const uzstd__u32 *cnt, int *last_sym, uzstd__u8 *len) {
    uzstd__u64 a[256];
    uzstd__u32 ncnt[512];
    short parent[512], depth[512];
    int m = 0, s, i, k, li, ni, nc, maxbits = 0;
    UZSTD_MEMSET(len, 0, 256);
    for (s = 0; s < 256; s++) if (cnt[s]) { a[m++] = ((uzstd__u64)cnt[s]<<9) | (uzstd__u32)s; *last_sym = s; }
    if (m < 2) return 0;
    for (i = 1; i < m; i++) { /* insertion sort ascending */
        uzstd__u64 v = a[i]; int j = i;
        while (j > 0 && a[j-1] > v) { a[j] = a[j-1]; j--; }
        a[j] = v;
    }
    for (i = 0; i < m; i++) ncnt[i] = (uzstd__u32)(a[i]>>9);
    li = 0; ni = nc = m;
    for (k = 0; k < m-1; k++) {
        int x, y;
        x = (li < m && (ni == nc || ncnt[li] <= ncnt[ni])) ? li++ : ni++;
        y = (li < m && (ni == nc || ncnt[li] <= ncnt[ni])) ? li++ : ni++;
        ncnt[nc] = ncnt[x] + ncnt[y]; parent[x] = (short)nc; parent[y] = (short)nc; nc++;
    }
    depth[nc-1] = 0;
    for (i = nc-2; i >= 0; i--) depth[i] = (short)(depth[parent[i]] + 1);
    for (i = 0; i < m; i++) {
        int L = depth[i] > 11 ? 11 : depth[i];
        len[a[i] & 511] = (uzstd__u8)L;
        if (L > maxbits) maxbits = L;
    }
    if (maxbits == 11) { /* repair kraft sum after capping */
        uzstd__u32 K = 0, C = 1u<<11;
        for (i = 0; i < m; i++) K += 1u << (11 - len[a[i] & 511]);
        while (K > C) /* lengthen rarest */
            for (i = 0; i < m && K > C; i++) {
                uzstd__u8 *L = &len[a[i] & 511];
                if (*L < 11) { K -= 1u << (10 - *L); (*L)++; }
            }
        while (K < C) /* shorten most frequent */
            for (i = m-1; i >= 0 && K < C; i--) {
                uzstd__u8 *L = &len[a[i] & 511];
                if (*L > 1 && K + (1u << (11 - *L)) <= C) { K += 1u << (11 - *L); (*L)--; }
            }
        maxbits = 0;
        for (i = 0; i < m; i++) if (len[a[i] & 511] > maxbits) maxbits = len[a[i] & 511];
    }
    return maxbits;
}

/* canonical codes matching zstd order: weight ascending, symbol ascending */
static int uzstd__huf_canonical(const uzstd__u8 *len, uzstd__u16 *code, int last_sym, int maxbits) {
    uzstd__u32 p = 0; int wv, s;
    for (wv = 1; wv <= maxbits; wv++)
        for (s = 0; s <= last_sym; s++) if (len[s] && maxbits+1-len[s] == wv) {
            code[s] = (uzstd__u16)(p >> (wv-1)); p += 1u << (wv-1);
        }
    return p == (1u<<maxbits);
}

/* tree description: FSE-compressed weights if smaller, else direct nibbles; 0 = fail */
static size_t uzstd__huf_tree(uzstd__u8 *d, size_t cap, const uzstd__u8 *len, int last_sym, int maxbits) {
    uzstd__u8 w[256];
    int s, nw = last_sym; /* explicit weights: symbols 0..last_sym-1, last is implied */
    size_t direct = 1 + (size_t)((nw+1)/2);
    if (cap < direct + 8 || cap < 136) return 0;
    for (s = 0; s < nw; s++) w[s] = len[s] ? (uzstd__u8)(maxbits+1-len[s]) : 0;
    if (nw >= 2) { /* FSE attempt (interleaved 2-state stream, accuracy <= 6) */
        uzstd__u32 cnt[16];
        short norm[16];
        int mw = 0, distinct = 0, log;
        UZSTD_MEMSET(cnt, 0, sizeof cnt);
        for (s = 0; s < nw; s++) cnt[w[s]]++;
        for (s = 0; s < 16; s++) if (cnt[s]) { distinct++; mw = s; }
        if (distinct >= 2) {
            uzstd__fse_ct ct;
            uzstd__bw bw;
            size_t ncsz, bssz;
            log = uzstd__highbit((uzstd__u32)(nw-1)) - 2;
            s = uzstd__highbit((uzstd__u32)mw) + 2;
            if (log < s) log = s;
            if (log < 5) log = 5;
            if (log > 6) log = 6;
            uzstd__fse_norm(cnt, mw, (uzstd__u32)nw, log, norm);
            ncsz = uzstd__fse_write_ncount(d+1, 127, norm, log);
            if (ncsz) {
                uzstd__fse_cs c1, c2;
                int ip = nw;
                uzstd__fse_build_ct(&ct, norm, mw, log);
                uzstd__bw_init(&bw, d+1+ncsz, d+1+127);
                if (nw & 1) {
                    uzstd__fse_init_state(&c1, &ct, w[--ip]);
                    uzstd__fse_init_state(&c2, &ct, w[--ip]);
                    uzstd__fse_encode(&bw, &c1, &ct, w[--ip]);
                } else {
                    uzstd__fse_init_state(&c2, &ct, w[--ip]);
                    uzstd__fse_init_state(&c1, &ct, w[--ip]);
                }
                while (ip > 0) {
                    uzstd__fse_encode(&bw, &c2, &ct, w[--ip]);
                    uzstd__fse_encode(&bw, &c1, &ct, w[--ip]);
                }
                uzstd__fse_flush_state(&bw, &c2, &ct);
                uzstd__fse_flush_state(&bw, &c1, &ct);
                bssz = uzstd__bw_close(&bw);
                if (bssz && ncsz + bssz <= 127 && (nw > 128 || 1 + ncsz + bssz < direct)) {
                    d[0] = (uzstd__u8)(ncsz + bssz);
                    return 1 + ncsz + bssz;
                }
            }
        }
    }
    if (nw > 128) return 0;
    d[0] = (uzstd__u8)(127 + nw);
    UZSTD_MEMSET(d+1, 0, (size_t)((nw+1)/2));
    for (s = 0; s < nw; s++) d[1 + (s>>1)] |= (uzstd__u8)(w[s] << ((s&1) ? 0 : 4));
    return direct;
}

/* one huffman stream (symbols encoded last-to-first); 0 = overflow */
static size_t uzstd__huf_stream(uzstd__u8 *d, uzstd__u8 *dend, const uzstd__u8 *src, size_t n,
                                const uzstd__u16 *code, const uzstd__u8 *len) {
    uzstd__bw bw;
    size_t i = n;
    uzstd__bw_init(&bw, d, dend);
    while (i > 0) { i--; uzstd__bw_add(&bw, code[src[i]], len[src[i]]); }
    return uzstd__bw_close(&bw);
}

/* ---------------- literals section ---------------- */
static size_t uzstd__lit_section(uzstd__ctx *cx, uzstd__u8 *d, size_t cap) {
    uzstd__u32 n = cx->nlit;
    const uzstd__u8 *L = cx->lit;
    size_t rawh = n < 32 ? 1 : n < 4096 ? 2 : 3;
    if (cap < rawh + n + 8) return 0;
    if (n >= 2) {
        uzstd__u32 cnt[256], i;
        uzstd__u16 code[256];
        uzstd__u8 len[256];
        int last_sym = 0, maxbits, distinct = 0;
        UZSTD_MEMSET(cnt, 0, sizeof cnt);
        for (i = 0; i < n; i++) cnt[L[i]]++;
        for (i = 0; i < 256; i++) if (cnt[i]) distinct++;
        if (distinct == 1) { /* RLE literals */
            if (rawh == 1) d[0] = (uzstd__u8)(1u | (n<<3));
            else if (rawh == 2) uzstd__wle16(d, 1u | (1u<<2) | (n<<4));
            else { d[0] = (uzstd__u8)(1u | (3u<<2) | (n<<4)); uzstd__wle16(d+1, n>>4); }
            d[rawh] = L[0];
            return rawh + 1;
        }
        if (n >= 64 && (maxbits = uzstd__huf_lengths(cnt, &last_sym, len)) != 0
            && uzstd__huf_canonical(len, code, last_sym, maxbits)) {
            uzstd__u64 bits = 0;
            for (i = 0; i <= (uzstd__u32)last_sym; i++) bits += (uzstd__u64)cnt[i]*len[i];
            if ((bits>>3) + 32 < n) {
                size_t hsize = n <= 1023 ? 3 : n <= 16383 ? 4 : 5, tsz, csize = 0;
                int four = n > 1023;
                uzstd__u8 *body = d + hsize, *bend = d + cap;
                tsz = uzstd__huf_tree(body, (size_t)(bend - body), len, last_sym, maxbits);
                if (tsz && !four) {
                    size_t ssz = uzstd__huf_stream(body+tsz, bend, L, n, code, len);
                    if (ssz) csize = tsz + ssz;
                } else if (tsz) {
                    uzstd__u32 q = (n+3)/4;
                    uzstd__u8 *p = body + tsz + 6;
                    size_t s1, s2, s3, s4;
                    s1 = uzstd__huf_stream(p, bend, L, q, code, len); p += s1;
                    s2 = s1 ? uzstd__huf_stream(p, bend, L+q, q, code, len) : 0; p += s2;
                    s3 = s2 ? uzstd__huf_stream(p, bend, L+2*q, q, code, len) : 0; p += s3;
                    s4 = s3 ? uzstd__huf_stream(p, bend, L+3*q, n-3*q, code, len) : 0;
                    if (s4 && s1 <= 0xFFFF && s2 <= 0xFFFF && s3 <= 0xFFFF) {
                        uzstd__wle16(body+tsz, (uzstd__u32)s1);
                        uzstd__wle16(body+tsz+2, (uzstd__u32)s2);
                        uzstd__wle16(body+tsz+4, (uzstd__u32)s3);
                        csize = tsz + 6 + s1 + s2 + s3 + s4;
                    }
                }
                if (csize && hsize + csize < rawh + n) {
                    if (hsize == 3) { uzstd__u32 v = 2u | (n<<4) | ((uzstd__u32)csize<<14);
                        d[0]=(uzstd__u8)v; uzstd__wle16(d+1, v>>8); }
                    else if (hsize == 4) uzstd__wle32(d, 2u | (2u<<2) | (n<<4) | ((uzstd__u32)csize<<18));
                    else { uzstd__u64 v = 2u | (3u<<2) | ((uzstd__u64)n<<4) | ((uzstd__u64)csize<<22);
                        uzstd__wle32(d, (uzstd__u32)v); d[4] = (uzstd__u8)(v>>32); }
                    return hsize + csize;
                }
            }
        }
    }
    if (rawh == 1) d[0] = (uzstd__u8)(n<<3);                  /* type=0 raw, 5-bit size */
    else if (rawh == 2) uzstd__wle16(d, (1u<<2) | (n<<4));    /* 12-bit size */
    else { d[0] = (uzstd__u8)((3u<<2) | (n<<4)); uzstd__wle16(d+1, n>>4); } /* 20-bit */
    UZSTD_MEMCPY(d + rawh, cx->lit, n);
    return rawh + n;
}

/* ---------------- frame / block driver ---------------- */
UZSTD_LINKAGE size_t uzstd_compress_bound(size_t n) {
    return UZSTD_COMPRESS_BOUND(n);
}

static size_t uzstd__frame_header(uzstd__u8 *dst, uzstd__u64 csize) {
    uzstd__u8 *d = dst;
    int fcs = csize >= 0xFFFFFFFFull ? 3 : csize > 0xFFFF ? 2 : csize > 0xFF ? 1 : 0;
    uzstd__wle32(d, 0xFD2FB528u); d += 4;
    *d++ = (uzstd__u8)((fcs<<6) | 0x20); /* single_segment=1, no checksum, no dict */
    switch (fcs) {
    case 0: *d++ = (uzstd__u8)csize; break;
    case 1: uzstd__wle16(d, (uzstd__u32)(csize-256)); d += 2; break;
    case 2: uzstd__wle32(d, (uzstd__u32)csize); d += 4; break;
    default: uzstd__wle32(d, (uzstd__u32)csize); uzstd__wle32(d+4, (uzstd__u32)(csize>>32)); d += 8;
    }
    return (size_t)(d - dst);
}

static void uzstd__block_header(uzstd__u8 *dst, int last, int type, uzstd__u32 size) {
    uzstd__u32 v = (size<<3) | ((uzstd__u32)type<<1) | (uzstd__u32)last;
    dst[0]=(uzstd__u8)v; dst[1]=(uzstd__u8)(v>>8); dst[2]=(uzstd__u8)(v>>16);
}

/* returns block content size (header at dst, content at dst+3) */
static size_t uzstd__compress_block(uzstd__ctx *cx, uzstd__u8 *dst, const uzstd__u8 *base,
                                    size_t bstart, size_t n, int last) {
    const uzstd__u8 *src = base + bstart;
    {   size_t i; /* RLE block */
        for (i = 1; i < n; i++) if (src[i] != src[0]) break;
        if (i == n && n > 1) { uzstd__block_header(dst, last, 1, (uzstd__u32)n); dst[3] = src[0]; return 1; }
    }
    if (n >= 16) {
        uzstd__parse(cx, base, bstart, n);
        {   uzstd__u8 *t = cx->tmp, *tend = cx->tmp + cx->tmp_cap;
            size_t sz = uzstd__lit_section(cx, t, (size_t)(tend - t));
            if (sz) {
                t += sz;
                sz = uzstd__encode_sequences(cx, t, tend);
                if (sz) {
                    size_t csz = (size_t)(t + sz - cx->tmp);
                    if (csz < n) {
                        uzstd__block_header(dst, last, 2, (uzstd__u32)csz);
                        UZSTD_MEMCPY(dst+3, cx->tmp, csz);
                        return csz;
                    }
                }
            }
        }
    }
    uzstd__block_header(dst, last, 0, (uzstd__u32)n);
    UZSTD_MEMCPY(dst+3, src, n);
    return n;
}

UZSTD_LINKAGE size_t uzstd_compress(void *dst_v, size_t dst_cap, const void *src_v, size_t src_size, int level) {
    uzstd__u8 *dst = (uzstd__u8*)dst_v, *d = dst;
    const uzstd__u8 *src = (const uzstd__u8*)src_v;
    size_t pos = 0;
    uzstd__ctx cx;
    uzstd__u8 *mem;
    if (dst_cap < UZSTD_COMPRESS_BOUND(src_size) || src_size >= 0x7FFFFFF0u) return (size_t)-1;
    d += uzstd__frame_header(d, src_size);
    if (src_size == 0) { uzstd__block_header(d, 1, 0, 0); return (size_t)(d + 3 - dst); }
    if (level < 1) level = 1;
    if (level > 9) level = 9;
    size_t bcap = src_size < UZSTD__BLOCK_MAX ? src_size : UZSTD__BLOCK_MAX;
    size_t mseq = bcap/3 + 8, hsize;
    int hbits = 8;
    while (hbits < 17 && ((size_t)1 << (hbits-1)) < src_size) hbits++;
    hsize = (size_t)1 << hbits;
    cx.hash_shift = 32 - hbits;
    cx.tmp_cap = bcap + 8192;
    mem = (uzstd__u8*)UZSTD_MALLOC((hsize + src_size)*4
                                + mseq*sizeof(uzstd__seq) + bcap + cx.tmp_cap);
    if (!mem) return (size_t)-1;
    cx.htab  = (uzstd__u32*)mem;
    cx.chain = cx.htab + hsize;
    cx.seqs  = (uzstd__seq*)(cx.chain + src_size);
    cx.lit   = (uzstd__u8*)(cx.seqs + mseq);
    cx.tmp   = cx.lit + bcap;
    UZSTD_MEMSET(cx.htab, 0, hsize*4);
    cx.rep[0] = 1; cx.rep[1] = 4; cx.rep[2] = 8;
    {   static const int depths[9] = {1,2,4,8,16,32,48,64,96};
        cx.depth = depths[level-1];
        cx.lazy = level >= 3;
    }
    while (pos < src_size) {
        size_t bn = src_size - pos > UZSTD__BLOCK_MAX ? UZSTD__BLOCK_MAX : src_size - pos;
        int last = pos + bn == src_size;
        d += 3 + uzstd__compress_block(&cx, d, src, pos, bn, last);
        pos += bn;
    }
    UZSTD_FREE(mem);
    return (size_t)(d - dst);
}


#endif /* UZSTD_COMPRESSOR_C */
#endif /* !UZSTD_NO_COMPRESSOR */

#if !defined(UZSTD_NO_DECOMPRESSOR)
#ifndef UZSTD_DECOMPRESSOR_C
#define UZSTD_DECOMPRESSOR_C

/* ================================================================== */
/* ============================ decompressor ============================= */
/* ================================================================== */

static uzstd__u32 uzstd__le32(const uzstd__u8 *p) {
    return (uzstd__u32)p[0] | ((uzstd__u32)p[1]<<8) | ((uzstd__u32)p[2]<<16) | ((uzstd__u32)p[3]<<24);
}
static uzstd__u64 uzstd__le64(const uzstd__u8 *p) {
    return (uzstd__u64)uzstd__le32(p) | ((uzstd__u64)uzstd__le32(p+4) << 32);
}

/* backward bitstream reader. p must have 8 readable bytes before it (block
** scratch front pad / preceding headers); reads use a +64-bit bias so byte
** indexing stays non-negative for look-ahead slightly past the stream start. */
typedef struct { const uzstd__u8 *p; long long pos; int err; } uzstd__br;

static int uzstd__br_init(uzstd__br *b, const uzstd__u8 *p, size_t n) {
    b->p = p; b->err = 0; b->pos = 0;
    if (!n || !p[n-1]) { b->err = 1; return 0; }
    b->pos = (long long)(n-1)*8 + uzstd__highbit(p[n-1]); /* strip end marker bit */
    return 1;
}
static uzstd__u32 uzstd__br_look(const uzstd__br *b, int nb) { /* nb <= 31 */
    uzstd__u64 x = (uzstd__u64)(b->pos - nb + 64);
    return (uzstd__u32)((uzstd__le64(b->p - 8 + (x>>3)) >> (x & 7)) & ((1u<<nb)-1));
}
static uzstd__u32 uzstd__br_read(uzstd__br *b, int nb) {
    uzstd__u32 v;
    if (nb > b->pos) { b->err = 1; b->pos = 0; return 0; }
    v = uzstd__br_look(b, nb);
    b->pos -= nb;
    return v;
}
static uzstd__u32 uzstd__fwd_bits(const uzstd__u8 *p, size_t bitpos, int nb) { /* forward LE reader */
    return (uzstd__u32)((uzstd__le64(p + (bitpos>>3)) >> (bitpos & 7)) & ((1u<<nb)-1));
}

/* FSE decode table entry / build (RFC 4.1.1) */
typedef struct { uzstd__u16 base; uzstd__u8 sym, nb; } uzstd__dte;

static int uzstd__fse_dtable(uzstd__dte *dt, const short *norm, int max_sym, int log) {
    int tsize = 1<<log, high = tsize-1, mask = tsize-1, step = (tsize>>1)+(tsize>>3)+3;
    uzstd__u16 next[64];
    int s, u, pos = 0;
    for (s = 0; s <= max_sym; s++) {
        if (norm[s] == -1) { dt[high--].sym = (uzstd__u8)s; next[s] = 1; }
        else next[s] = (uzstd__u16)norm[s];
    }
    for (s = 0; s <= max_sym; s++) {
        int i;
        for (i = 0; i < norm[s]; i++) {
            dt[pos].sym = (uzstd__u8)s;
            do { pos = (pos + step) & mask; } while (pos > high);
        }
    }
    if (pos != 0) return 0;
    for (u = 0; u < tsize; u++) {
        int ns = next[dt[u].sym]++;
        dt[u].nb = (uzstd__u8)(log - uzstd__highbit((uzstd__u32)ns));
        dt[u].base = (uzstd__u16)((ns << dt[u].nb) - tsize);
    }
    return 1;
}

/* read FSE table description; norm[0..max_sym] must be pre-zeroed.
** returns bytes consumed, 0 on corruption. */
static size_t uzstd__read_ncount(short *norm, int max_sym, int max_log, int *log_out,
                                 const uzstd__u8 *ip, size_t avail) {
    size_t bp = 4;
    int log, remaining, threshold, nbits, sym = 0, prev0 = 0;
    if (!avail) return 0;
    log = (ip[0] & 15) + 5;
    if (log > max_log) return 0;
    *log_out = log;
    remaining = (1<<log) + 1; threshold = 1<<log; nbits = log + 1;
    while (remaining > 1) {
        if (prev0) {
            while (uzstd__fwd_bits(ip, bp, 16) == 0xFFFF) {
                sym += 24; bp += 16;
                if (sym > max_sym || (bp>>3) > avail) return 0;
            }
            while (uzstd__fwd_bits(ip, bp, 2) == 3) { sym += 3; bp += 2; if (sym > max_sym) return 0; }
            sym += (int)uzstd__fwd_bits(ip, bp, 2); bp += 2;
            if (sym > max_sym) return 0;
        }
        {   int max = (2*threshold-1) - remaining, count;
            if ((int)uzstd__fwd_bits(ip, bp, nbits-1) < max) {
                count = (int)uzstd__fwd_bits(ip, bp, nbits-1); bp += (size_t)(nbits-1);
            } else {
                count = (int)uzstd__fwd_bits(ip, bp, nbits); bp += (size_t)nbits;
                if (count >= threshold) count -= max;
            }
            count--; /* 0 => -1 (low prob), 1 => 0, ... */
            remaining -= count < 0 ? -count : count;
            if (remaining < 1 || sym > max_sym) return 0;
            norm[sym++] = (short)count;
            prev0 = (count == 0);
            while (remaining < threshold) { nbits--; threshold >>= 1; }
        }
        if ((bp>>3) > avail) return 0;
    }
    bp = (bp + 7) >> 3;
    return bp <= avail ? bp : 0;
}

/* decoder context: tables persist across blocks within a frame */
typedef struct {
    uzstd__dte dll[1<<9], dml[1<<9], dof[1<<8];
    int tlog[3], tok[3];                /* ll/of/ml table logs + repeat-valid flags */
    uzstd__u16 huf[1<<11];
    int huf_log, huf_ok;
    uzstd__u32 rep[3];
    uzstd__u8 lit[131072];              /* decoded literals */
    uzstd__u8 blk[8 + 131072 + 8];      /* padded copy of block content */
} uzstd__dctx;

/* Huffman tree description -> cx->huf decode table; returns bytes consumed, 0 = fail */
static size_t uzstd__huf_tree_d(uzstd__dctx *cx, const uzstd__u8 *ip, size_t avail) {
    uzstd__u8 w[256];
    uzstd__u32 sum = 0, rest;
    int nw, i, tlog;
    size_t used;
    if (!avail) return 0;
    if (ip[0] >= 128) { /* direct: 4-bit weights */
        nw = ip[0] - 127;
        used = 1 + (size_t)((nw+1)/2);
        if (used > avail) return 0;
        for (i = 0; i < nw; i++) w[i] = (i&1) ? (ip[1+(i>>1)] & 15) : (ip[1+(i>>1)] >> 4);
    } else { /* FSE-compressed weights, two interleaved states */
        short norm[16];
        uzstd__dte dt[64];
        uzstd__br b;
        int log, s1, s2, n = 0;
        size_t csz = ip[0], nc;
        if (1 + csz > avail) return 0;
        UZSTD_MEMSET(norm, 0, sizeof norm);
        nc = uzstd__read_ncount(norm, 15, 6, &log, ip+1, csz);
        if (!nc || !uzstd__fse_dtable(dt, norm, 15, log)) return 0;
        if (!uzstd__br_init(&b, ip+1+nc, csz-nc)) return 0;
        s1 = (int)uzstd__br_read(&b, log);
        s2 = (int)uzstd__br_read(&b, log);
        if (b.err) return 0;
        while (1) { /* decode until bitstream exhausted */
            int t;
            if (n > 254) return 0;
            w[n++] = dt[s1].sym;
            if (dt[s1].nb > b.pos) { if (n > 254) return 0; w[n++] = dt[s2].sym; break; }
            s1 = dt[s1].base + (int)uzstd__br_read(&b, dt[s1].nb);
            t = s1; s1 = s2; s2 = t;
        }
        nw = n;
        used = 1 + csz;
    }
    for (i = 0; i < nw; i++) { if (w[i] > 11) return 0; if (w[i]) sum += 1u << (w[i]-1); }
    if (!sum) return 0;
    tlog = uzstd__highbit(sum) + 1;
    rest = (1u<<tlog) - sum;
    if (tlog > 11 || (rest & (rest-1))) return 0; /* must complete to a power of 2 */
    w[nw++] = (uzstd__u8)(uzstd__highbit(rest) + 1); /* implied last weight */
    {   uzstd__u32 p = 0; int wv, s; /* fill order: weight asc, symbol asc */
        for (wv = 1; wv <= tlog; wv++)
            for (s = 0; s < nw; s++) if (w[s] == wv) {
                uzstd__u32 k, cnt = 1u << (wv-1), e = (uzstd__u32)s | ((uzstd__u32)(tlog+1-wv) << 8);
                for (k = 0; k < cnt; k++) cx->huf[p++] = (uzstd__u16)e;
            }
    }
    cx->huf_log = tlog; cx->huf_ok = 1;
    return used;
}

static int uzstd__huf_stream_d(uzstd__dctx *cx, uzstd__u8 *out, size_t n, const uzstd__u8 *p, size_t sz) {
    uzstd__br b;
    int log = cx->huf_log;
    size_t i;
    if (!uzstd__br_init(&b, p, sz)) return 0;
    for (i = 0; i < n; i++) {
        uzstd__u32 e = cx->huf[uzstd__br_look(&b, log)];
        b.pos -= (int)(e >> 8);
        out[i] = (uzstd__u8)e;
        if (b.pos < 0) return 0;
    }
    return b.pos == 0; /* stream must be fully consumed */
}

/* literals section; sets lit ptr + size, returns bytes consumed, 0 = fail */
static size_t uzstd__lits_d(uzstd__dctx *cx, const uzstd__u8 *ip, size_t avail,
                            const uzstd__u8 **litp, size_t *litsz) {
    int type, sf;
    size_t regen, h;
    if (!avail) return 0;
    type = ip[0] & 3; sf = (ip[0] >> 2) & 3;
    if (type <= 1) { /* raw / RLE */
        if (sf == 1)      { if (avail < 2) return 0; h = 2; regen = (((size_t)ip[0] | ((size_t)ip[1]<<8)) >> 4); }
        else if (sf == 3) { if (avail < 3) return 0; h = 3; regen = (((size_t)ip[0] | ((size_t)ip[1]<<8) | ((size_t)ip[2]<<16)) >> 4); }
        else              { h = 1; regen = (size_t)(ip[0] >> 3); }
        if (regen > 131072) return 0;
        *litsz = regen;
        if (type == 0) {
            if (h + regen > avail) return 0;
            *litp = ip + h; /* point into block scratch, no copy */
            return h + regen;
        }
        if (h + 1 > avail) return 0;
        UZSTD_MEMSET(cx->lit, ip[h], regen);
        *litp = cx->lit;
        return h + 1;
    }
    {   /* compressed (2) / treeless (3) */
        const uzstd__u8 *p;
        size_t csz, rem, used;
        int four = (sf != 0);
        if (sf <= 1) { if (avail < 3) return 0; h = 3;
            { uzstd__u32 v = (uzstd__u32)ip[0] | ((uzstd__u32)ip[1]<<8) | ((uzstd__u32)ip[2]<<16);
              regen = (v>>4) & 0x3FF; csz = v >> 14; } }
        else if (sf == 2) { if (avail < 4) return 0; h = 4;
            { uzstd__u32 v = uzstd__le32(ip); regen = (v>>4) & 0x3FFF; csz = v >> 18; } }
        else { if (avail < 5) return 0; h = 5;
            { uzstd__u64 v = (uzstd__u64)uzstd__le32(ip) | ((uzstd__u64)ip[4]<<32);
              regen = (size_t)((v>>4) & 0x3FFFF); csz = (size_t)(v >> 22); } }
        if (!regen || regen > 131072 || !csz || h + csz > avail) return 0;
        p = ip + h; rem = csz;
        if (type == 2) {
            used = uzstd__huf_tree_d(cx, p, rem);
            if (!used) return 0;
            p += used; rem -= used;
        } else if (!cx->huf_ok) return 0; /* treeless without previous tree */
        if (!four) {
            if (!uzstd__huf_stream_d(cx, cx->lit, regen, p, rem)) return 0;
        } else {
            size_t q = (regen + 3) >> 2, sz1, sz2, sz3, sz4;
            if (3*q >= regen || rem < 6) return 0; /* 4 streams need >= 4 literals */
            sz1 = (size_t)p[0] | ((size_t)p[1]<<8);
            sz2 = (size_t)p[2] | ((size_t)p[3]<<8);
            sz3 = (size_t)p[4] | ((size_t)p[5]<<8);
            p += 6; rem -= 6;
            if (sz1 + sz2 + sz3 + 1 > rem) return 0;
            sz4 = rem - sz1 - sz2 - sz3;
            if (!uzstd__huf_stream_d(cx, cx->lit,       q,           p,             sz1)) return 0;
            if (!uzstd__huf_stream_d(cx, cx->lit + q,   q,           p+sz1,         sz2)) return 0;
            if (!uzstd__huf_stream_d(cx, cx->lit + 2*q, q,           p+sz1+sz2,     sz3)) return 0;
            if (!uzstd__huf_stream_d(cx, cx->lit + 3*q, regen - 3*q, p+sz1+sz2+sz3, sz4)) return 0;
        }
        *litp = cx->lit; *litsz = regen;
        return h + csz;
    }
}

/* RFC 3.1.1.3.2.2: predefined distributions */
static const short uzstd__llnorm[36] = {4,3,2,2,2,2,2,2,2,2,2,2,2,1,1,1,
    2,2,2,2,2,2,2,2,2,3,2,1,1,1,1,1,-1,-1,-1,-1};
static const short uzstd__ofnorm[29] = {1,1,1,1,1,1,2,2,2,1,1,1,1,1,1,1,
    1,1,1,1,1,1,1,1,-1,-1,-1,-1,-1};
static const short uzstd__mlnorm[53] = {1,4,3,2,2,2,2,2,2,1,1,1,1,1,1,1,
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,-1,-1,-1,-1,-1,-1,-1};

/* prepare one sequence decode table per its mode; returns bytes consumed or -1 */
static size_t uzstd__seq_prep(uzstd__dctx *cx, int k /*0=ll 1=of 2=ml*/, int mode,
                              const uzstd__u8 *ip, size_t avail) {
    static const short * const dnorm[3] = { uzstd__llnorm, uzstd__ofnorm, uzstd__mlnorm };
    static const uzstd__u8 dlog[3] = {6,5,6}, dmxs[3] = {35,28,52}, mxs[3] = {35,31,52}, mxl[3] = {9,8,9};
    uzstd__dte *dt = k == 0 ? cx->dll : k == 1 ? cx->dof : cx->dml;
    switch (mode) {
    case 0: /* predefined */
        if (!uzstd__fse_dtable(dt, dnorm[k], dmxs[k], dlog[k])) return (size_t)-1;
        cx->tlog[k] = dlog[k]; cx->tok[k] = 1;
        return 0;
    case 1: /* RLE: one byte = the code */
        if (!avail || ip[0] > mxs[k]) return (size_t)-1;
        dt[0].sym = ip[0]; dt[0].nb = 0; dt[0].base = 0;
        cx->tlog[k] = 0; cx->tok[k] = 1;
        return 1;
    case 2: { /* FSE table description */
        short norm[64];
        int log;
        size_t nc;
        UZSTD_MEMSET(norm, 0, sizeof norm);
        nc = uzstd__read_ncount(norm, mxs[k], mxl[k], &log, ip, avail);
        if (!nc || !uzstd__fse_dtable(dt, norm, mxs[k], log)) return (size_t)-1;
        cx->tlog[k] = log; cx->tok[k] = 1;
        return nc; }
    default: /* repeat previous */
        return cx->tok[k] ? 0 : (size_t)-1;
    }
}

/* decode one compressed block (content already copied into padded scratch) */
static int uzstd__block_d(uzstd__dctx *cx, uzstd__u8 **op_io, uzstd__u8 *oend,
                          const uzstd__u8 *frame_base, const uzstd__u8 *ip, size_t n) {
    const uzstd__u8 *iend = ip + n, *lp, *lend;
    uzstd__u8 *op = *op_io, *ostart = op;
    size_t litsz, used, ns, i;
    used = uzstd__lits_d(cx, ip, n, &lp, &litsz);
    if (!used) return 0;
    ip += used; lend = lp + litsz;
    if (ip >= iend) return 0;
    if (ip[0] < 128)      { ns = ip[0]; ip += 1; }
    else if (ip[0] < 255) { if (iend - ip < 2) return 0; ns = ((size_t)(ip[0]-128)<<8) + ip[1]; ip += 2; }
    else                  { if (iend - ip < 3) return 0; ns = ((size_t)ip[1] | ((size_t)ip[2]<<8)) + 0x7F00; ip += 3; }
    if (ns == 0) {
        if (ip != iend) return 0;
    } else {
        uzstd__br b;
        uzstd__u32 sll, sof, sml;
        int modes;
        if (ip >= iend) return 0; /* need the sequence-modes byte */
        modes = *ip++;
        if (modes & 3) return 0; /* reserved bits */
        for (i = 0; i < 3; i++) {
            used = uzstd__seq_prep(cx, (int)i, (modes >> (6 - 2*(int)i)) & 3, ip, (size_t)(iend - ip));
            if (UZSTD_IS_ERROR(used) || used > (size_t)(iend - ip)) return 0;
            ip += used;
        }
        if (!uzstd__br_init(&b, ip, (size_t)(iend - ip))) return 0;
        sll = uzstd__br_read(&b, cx->tlog[0]);
        sof = uzstd__br_read(&b, cx->tlog[1]);
        sml = uzstd__br_read(&b, cx->tlog[2]);
        for (i = 0; i < ns; i++) {
            uzstd__dte ell = cx->dll[sll], eof = cx->dof[sof], eml = cx->dml[sml];
            uzstd__u32 ov, ll, ml, off;
            ov = (1u << eof.sym) + uzstd__br_read(&b, eof.sym);
            ml = 3 + uzstd__mlbase[eml.sym] + uzstd__br_read(&b, uzstd__mlbits[eml.sym]);
            ll = uzstd__llbase[ell.sym] + uzstd__br_read(&b, uzstd__llbits[ell.sym]);
            if (ov > 3) {
                off = ov - 3;
                cx->rep[2] = cx->rep[1]; cx->rep[1] = cx->rep[0]; cx->rep[0] = off;
            } else {
                uzstd__u32 idx = ov - 1 + (ll == 0);
                if (idx == 3) { off = cx->rep[0] - 1; if (!off) return 0; }
                else off = cx->rep[idx];
                if (idx) {
                    if (idx > 1) cx->rep[2] = cx->rep[1];
                    cx->rep[1] = cx->rep[0]; cx->rep[0] = off;
                }
            }
            if (i + 1 < ns) { /* no state update after last sequence */
                sll = ell.base + uzstd__br_read(&b, ell.nb);
                sml = eml.base + uzstd__br_read(&b, eml.nb);
                sof = eof.base + uzstd__br_read(&b, eof.nb);
            }
            if (b.err) return 0;
            if (ll > (size_t)(lend - lp) || (size_t)ll + ml > (size_t)(oend - op)) return 0;
            UZSTD_MEMCPY(op, lp, ll); op += ll; lp += ll;
            if (off > (size_t)(op - frame_base)) return 0;
            {   const uzstd__u8 *mp = op - off;
                uzstd__u8 *e = op + ml;
                if (off >= 8 && (size_t)(oend - op) >= (size_t)ml + 8) {
                    do { UZSTD_MEMCPY(op, mp, 8); op += 8; mp += 8; } while (op < e);
                    op = e;
                } else while (op < e) *op++ = *mp++;
            }
        }
        if (b.err || b.pos != 0) return 0; /* bitstream must be fully consumed */
    }
    used = (size_t)(lend - lp); /* trailing literals */
    if (used > (size_t)(oend - op)) return 0;
    UZSTD_MEMCPY(op, lp, used); op += used;
    if ((size_t)(op - ostart) > 131072) return 0; /* Block_Maximum_Size */
    *op_io = op;
    return 1;
}

UZSTD_LINKAGE unsigned long long uzstd_frame_content_size(const void *src_v, size_t n) {
    const uzstd__u8 *ip = (const uzstd__u8*)src_v, *iend = ip + n;
    while (iend - ip >= 8 && (uzstd__le32(ip) & 0xFFFFFFF0u) == 0x184D2A50u) {
        size_t sk = uzstd__le32(ip+4);
        if ((size_t)(iend - ip) - 8 < sk) return (unsigned long long)-1;
        ip += 8 + sk;
    }
    if (iend - ip < 6 || uzstd__le32(ip) != 0xFD2FB528u) return (unsigned long long)-1;
    {   int fhd = ip[4], fcsf = fhd >> 6, ss = (fhd >> 5) & 1;
        static const int dsz[4] = {0,1,2,4};
        ip += 5 + (ss ? 0 : 1) + dsz[fhd & 3];
        if (fcsf == 0 && !ss) return (unsigned long long)-1; /* unknown */
        switch (fcsf) {
        case 0:  return iend - ip < 1 ? (unsigned long long)-1 : ip[0];
        case 1:  return iend - ip < 2 ? (unsigned long long)-1 : 256ull + ((uzstd__u32)ip[0] | ((uzstd__u32)ip[1]<<8));
        case 2:  return iend - ip < 4 ? (unsigned long long)-1 : uzstd__le32(ip);
        default: return iend - ip < 8 ? (unsigned long long)-1 : uzstd__le64(ip);
        }
    }
}

UZSTD_LINKAGE size_t uzstd_decompress(void *dst_v, size_t dst_cap, const void *src_v, size_t src_size) {
    const uzstd__u8 *ip = (const uzstd__u8*)src_v, *iend = ip + src_size;
    uzstd__u8 *op = (uzstd__u8*)dst_v, *oend = op + dst_cap;
    uzstd__dctx *cx = 0;
    while (iend - ip >= 4) {
        if ((uzstd__le32(ip) & 0xFFFFFFF0u) == 0x184D2A50u) { /* skippable frame */
            size_t sk;
            if (iend - ip < 8) goto fail;
            sk = uzstd__le32(ip+4);
            if ((size_t)(iend - ip) - 8 < sk) goto fail;
            ip += 8 + sk;
            continue;
        }
        if (uzstd__le32(ip) != 0xFD2FB528u) goto fail;
        ip += 4;
        {   uzstd__u64 fcs = (uzstd__u64)-1;
            uzstd__u8 *fstart = op;
            int fhd, fcsf, ss, ck;
            if (ip >= iend) goto fail;
            fhd = *ip++;
            if (fhd & 8) goto fail; /* reserved bit */
            fcsf = fhd >> 6; ss = (fhd >> 5) & 1; ck = (fhd >> 2) & 1;
            if (!ss) { if (ip >= iend) goto fail; ip++; } /* window descriptor (unused: single-shot) */
            {   static const int dsz[4] = {0,1,2,4}; /* dictionary id: must be absent or zero */
                int k, nb = dsz[fhd & 3];
                uzstd__u32 did = 0;
                if (iend - ip < nb) goto fail;
                for (k = 0; k < nb; k++) did |= (uzstd__u32)ip[k] << (8*k);
                if (did) goto fail; /* dictionaries unsupported */
                ip += nb;
            }
            {   int nb = fcsf == 0 ? ss : fcsf == 1 ? 2 : fcsf == 2 ? 4 : 8, k;
                if (iend - ip < nb) goto fail;
                if (nb) {
                    fcs = 0;
                    for (k = 0; k < nb; k++) fcs |= (uzstd__u64)ip[k] << (8*k);
                    if (fcsf == 1) fcs += 256;
                    ip += nb;
                }
            }
            if (!cx) { cx = (uzstd__dctx*)UZSTD_MALLOC(sizeof *cx); if (!cx) goto fail; }
            cx->huf_ok = 0; cx->tok[0] = cx->tok[1] = cx->tok[2] = 0;
            cx->rep[0] = 1; cx->rep[1] = 4; cx->rep[2] = 8;
            for (;;) {
                uzstd__u32 bh;
                int last, type;
                size_t bsz;
                if (iend - ip < 3) goto fail;
                bh = (uzstd__u32)ip[0] | ((uzstd__u32)ip[1]<<8) | ((uzstd__u32)ip[2]<<16);
                ip += 3;
                last = bh & 1; type = (bh >> 1) & 3; bsz = bh >> 3;
                if (type == 3 || bsz > 131072) goto fail;
                if (type == 0) {
                    if ((size_t)(iend - ip) < bsz || (size_t)(oend - op) < bsz) goto fail;
                    UZSTD_MEMCPY(op, ip, bsz); op += bsz; ip += bsz;
                } else if (type == 1) {
                    if (iend - ip < 1 || (size_t)(oend - op) < bsz) goto fail;
                    UZSTD_MEMSET(op, ip[0], bsz); op += bsz; ip += 1;
                } else {
                    if (!bsz || (size_t)(iend - ip) < bsz) goto fail;
                    UZSTD_MEMSET(cx->blk, 0, 8);
                    UZSTD_MEMCPY(cx->blk + 8, ip, bsz);
                    UZSTD_MEMSET(cx->blk + 8 + bsz, 0, 8);
                    if (!uzstd__block_d(cx, &op, oend, fstart, cx->blk + 8, bsz)) goto fail;
                    ip += bsz;
                }
                if (last) break;
            }
            if (fcs != (uzstd__u64)-1 && (uzstd__u64)(op - fstart) != fcs) goto fail;
            if (ck) { if (iend - ip < 4) goto fail; ip += 4; } /* checksum skipped, not verified */
        }
    }
    if (ip != iend) goto fail;
    UZSTD_FREE(cx);
    return (size_t)(op - (uzstd__u8*)dst_v);
fail:
    UZSTD_FREE(cx);
    return (size_t)-1;
}


#endif /* UZSTD_DECOMPRESSOR_C */
#endif /* !UZSTD_NO_DECOMPRESSOR */

#endif /* UZSTD_IMPLEMENTATION */
