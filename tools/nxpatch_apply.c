/*
 * Standalone C11 NXPT (*.nxpatch) applier & SHA-256 verifier.
 * Reusable directly on both Linux host (Gate 2 parity test) and Android ARM64 (Gate 3 native extractor).
 * Zero external library dependencies: self-contained LZ4 block decompressor + FIPS 180-4 SHA-256.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------- Minimal FIPS 180-4 SHA-256 ---------------- */
typedef struct { uint32_t s[8]; uint64_t bitlen; uint8_t buf[64]; size_t buflen; } sha256_ctx;
static const uint32_t K256[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};
static inline uint32_t rotr32(uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); }
static void sha256_block(sha256_ctx *c, const uint8_t b[64]) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)b[i*4]<<24)|((uint32_t)b[i*4+1]<<16)|((uint32_t)b[i*4+2]<<8)|b[i*4+3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr32(w[i-15],7) ^ rotr32(w[i-15],18) ^ (w[i-15]>>3);
        uint32_t s1 = rotr32(w[i-2],17) ^ rotr32(w[i-2],19) ^ (w[i-2]>>10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint32_t a=c->s[0],b0=c->s[1],cc=c->s[2],d=c->s[3],e=c->s[4],f=c->s[5],g=c->s[6],h=c->s[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = rotr32(e,6)^rotr32(e,11)^rotr32(e,25);
        uint32_t ch = (e&f)^((~e)&g);
        uint32_t t1 = h + S1 + ch + K256[i] + w[i];
        uint32_t S0 = rotr32(a,2)^rotr32(a,13)^rotr32(a,22);
        uint32_t maj = (a&b0)^(a&cc)^(b0&cc);
        uint32_t t2 = S0 + maj;
        h=g; g=f; f=e; e=d+t1; d=cc; cc=b0; b0=a; a=t1+t2;
    }
    c->s[0]+=a; c->s[1]+=b0; c->s[2]+=cc; c->s[3]+=d;
    c->s[4]+=e; c->s[5]+=f; c->s[6]+=g; c->s[7]+=h;
}
static void sha256_init(sha256_ctx *c) {
    c->s[0]=0x6a09e667; c->s[1]=0xbb67ae85; c->s[2]=0x3c6ef372; c->s[3]=0xa54ff53a;
    c->s[4]=0x510e527f; c->s[5]=0x9b05688c; c->s[6]=0x1f83d9ab; c->s[7]=0x5be0cd19;
    c->bitlen = 0; c->buflen = 0;
}
static void sha256_update(sha256_ctx *c, const uint8_t *data, size_t len) {
    c->bitlen += (uint64_t)len * 8;
    while (len > 0) {
        size_t take = 64 - c->buflen;
        if (take > len) take = len;
        memcpy(c->buf + c->buflen, data, take);
        c->buflen += take; data += take; len -= take;
        if (c->buflen == 64) { sha256_block(c, c->buf); c->buflen = 0; }
    }
}
static void sha256_final(sha256_ctx *c, uint8_t out[32]) {
    uint64_t bl = c->bitlen;
    uint8_t pad = 0x80;
    sha256_update(c, &pad, 1);
    uint8_t z = 0;
    while (c->buflen != 56) sha256_update(c, &z, 1);
    uint8_t lb[8];
    for (int i = 0; i < 8; i++) lb[i] = (uint8_t)(bl >> (56 - i * 8));
    sha256_update(c, lb, 8);
    for (int i = 0; i < 8; i++) {
        out[i*4]   = (uint8_t)(c->s[i]>>24); out[i*4+1] = (uint8_t)(c->s[i]>>16);
        out[i*4+2] = (uint8_t)(c->s[i]>>8);  out[i*4+3] = (uint8_t)(c->s[i]);
    }
}
static void sha256_buf(const uint8_t *data, size_t len, uint8_t out[32]) {
    sha256_ctx c; sha256_init(&c); sha256_update(&c, data, len); sha256_final(&c, out);
}

/* ---------------- LZ4 Block Decompressor ---------------- */
static int lz4_decompress_block(const uint8_t *src, size_t src_len, uint8_t *dst, size_t dst_cap) {
    size_t sp = 0, dp = 0;
    while (sp < src_len) {
        uint8_t tok = src[sp++];
        size_t ll = tok >> 4;
        if (ll == 15) {
            uint8_t b;
            do { if (sp >= src_len) return -1; b = src[sp++]; ll += b; } while (b == 255);
        }
        if (sp + ll > src_len || dp + ll > dst_cap) return -2;
        if (ll) { memcpy(dst + dp, src + sp, ll); sp += ll; dp += ll; }
        if (sp >= src_len) break;
        if (sp + 2 > src_len) return -3;
        size_t off = (size_t)src[sp] | ((size_t)src[sp + 1] << 8);
        sp += 2;
        if (off == 0 || off > dp) return -4;
        size_t ml = (tok & 0x0F) + 4;
        if ((tok & 0x0F) == 15) {
            uint8_t b;
            do { if (sp >= src_len) return -5; b = src[sp++]; ml += b; } while (b == 255);
        }
        if (dp + ml > dst_cap) return -6;
        size_t ref = dp - off;
        for (size_t k = 0; k < ml; k++) dst[dp++] = dst[ref + k];
    }
    return dp == dst_cap ? 0 : -7;
}

static inline uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}
static inline uint64_t rd64(const uint8_t *p) {
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}
static uint8_t *read_all(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    rewind(f);
    uint8_t *b = malloc((size_t)sz);
    if (!b) { fclose(f); return NULL; }
    if (sz > 0 && fread(b, 1, (size_t)sz, f) != (size_t)sz) { free(b); fclose(f); return NULL; }
    fclose(f);
    *out_len = (size_t)sz;
    return b;
}

int nxpatch_apply_file(const char *raw_nso_path, const char *nxpatch_path, const char *out_path, char *errbuf, size_t errcap) {
    size_t nso_len = 0, pat_len = 0;
    uint8_t *nso = read_all(raw_nso_path, &nso_len);
    if (!nso) { snprintf(errbuf, errcap, "cannot read NSO: %s", raw_nso_path); return -1; }
    uint8_t *pat = read_all(nxpatch_path, &pat_len);
    if (!pat) { free(nso); snprintf(errbuf, errcap, "cannot read nxpatch: %s", nxpatch_path); return -2; }

    if (pat_len < 160 || memcmp(pat, "NXPT", 4) != 0 || rd32(pat + 4) != 1) {
        free(nso); free(pat); snprintf(errbuf, errcap, "invalid NXPT header"); return -3;
    }
    uint32_t pflags = rd32(pat + 8);
    uint32_t num_hunks = rd32(pat + 12);
    const uint8_t *exp_bid = pat + 16;
    const uint8_t *exp_raw_sha = pat + 32;
    const uint8_t *exp_base_sha = pat + 64;
    const uint8_t *exp_out_sha = pat + 96;
    uint64_t base_img_len = rd64(pat + 128);
    uint64_t out_total_len = rd64(pat + 136);
    uint64_t tail_off = rd64(pat + 144);
    uint64_t tail_len = rd64(pat + 152);

    if (nso_len < 0x100 || memcmp(nso, "NSO0", 4) != 0) {
        free(nso); free(pat); snprintf(errbuf, errcap, "invalid NSO0 header"); return -4;
    }
    if (memcmp(nso + 0x40, exp_bid, 16) != 0) {
        free(nso); free(pat); snprintf(errbuf, errcap, "NSO Build ID mismatch"); return -5;
    }
    uint8_t got_sha[32];
    sha256_buf(nso, nso_len, got_sha);
    if (memcmp(got_sha, exp_raw_sha, 32) != 0) {
        free(nso); free(pat); snprintf(errbuf, errcap, "raw NSO SHA-256 mismatch"); return -6;
    }

    int far = (pflags & 1) != 0;
    uint32_t nso_flags = rd32(nso + 0x0C);
    uint32_t seg_fo[3], seg_mo[3], seg_sz[3], seg_csz[3];
    for (int i = 0; i < 3; i++) {
        seg_fo[i]  = rd32(nso + 0x10 + i * 0x10);
        seg_mo[i]  = rd32(nso + 0x14 + i * 0x10);
        seg_sz[i]  = rd32(nso + 0x18 + i * 0x10);
        seg_csz[i] = rd32(nso + 0x60 + i * 4);
    }
    uint32_t bss = rd32(nso + 0x3C);
    uint64_t calc_base = (uint64_t)seg_mo[2] + seg_sz[2] + (far ? 0 : bss);
    if (calc_base != base_img_len || out_total_len < base_img_len || tail_off > out_total_len || tail_off + tail_len != out_total_len) {
        free(nso); free(pat); snprintf(errbuf, errcap, "layout size mismatch"); return -7;
    }

    uint8_t *out = calloc(1, (size_t)out_total_len);
    if (!out) { free(nso); free(pat); snprintf(errbuf, errcap, "OOM allocating %llu bytes", (unsigned long long)out_total_len); return -8; }

    for (int i = 0; i < 3; i++) {
        if ((uint64_t)seg_fo[i] + seg_csz[i] > nso_len || (uint64_t)seg_mo[i] + seg_sz[i] > base_img_len) {
            free(out); free(nso); free(pat); snprintf(errbuf, errcap, "NSO segment %d out of bounds", i); return -9;
        }
        if ((nso_flags >> i) & 1) {
            int rc = lz4_decompress_block(nso + seg_fo[i], seg_csz[i], out + seg_mo[i], seg_sz[i]);
            if (rc != 0) { free(out); free(nso); free(pat); snprintf(errbuf, errcap, "LZ4 decompress seg %d failed (%d)", i, rc); return -10; }
        } else {
            memcpy(out + seg_mo[i], nso + seg_fo[i], seg_sz[i]);
        }
    }
    free(nso);

    sha256_buf(out, (size_t)base_img_len, got_sha);
    if (memcmp(got_sha, exp_base_sha, 32) != 0) {
        free(out); free(pat); snprintf(errbuf, errcap, "decompressed base image SHA-256 mismatch"); return -11;
    }

    size_t pos = 160;
    for (uint32_t h = 0; h < num_hunks; h++) {
        if (pos + 8 > pat_len) { free(out); free(pat); snprintf(errbuf, errcap, "truncated hunk header %u", h); return -12; }
        uint32_t off = rd32(pat + pos);
        uint32_t len = rd32(pat + pos + 4);
        pos += 8;
        if (pos + len > pat_len || (uint64_t)off + len > base_img_len) {
            free(out); free(pat); snprintf(errbuf, errcap, "hunk %u out of bounds", h); return -13;
        }
        memcpy(out + off, pat + pos, len);
        pos += len;
    }
    if (pos + tail_len != pat_len) {
        free(out); free(pat); snprintf(errbuf, errcap, "tail length mismatch (pos=%zu tail=%llu pat=%zu)", pos, (unsigned long long)tail_len, pat_len); return -14;
    }
    if (tail_len) memcpy(out + tail_off, pat + pos, (size_t)tail_len);

    sha256_buf(out, (size_t)out_total_len, got_sha);
    int sha_ok = memcmp(got_sha, exp_out_sha, 32) == 0;
    free(pat);
    if (!sha_ok) {
        free(out); snprintf(errbuf, errcap, "patched output SHA-256 verification failed"); return -15;
    }

    FILE *wf = fopen(out_path, "wb");
    if (!wf) { free(out); snprintf(errbuf, errcap, "cannot create output: %s", out_path); return -16; }
    size_t nw = fwrite(out, 1, (size_t)out_total_len, wf);
    fclose(wf);
    free(out);
    if (nw != (size_t)out_total_len) { snprintf(errbuf, errcap, "short write on output"); return -17; }
    return 0;
}

#ifndef NXPATCH_LIB_ONLY
int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: nxpatch_apply <raw_nso> <file.nxpatch> <out.patched.bin>\n");
        return 2;
    }
    char err[256] = "";
    int rc = nxpatch_apply_file(argv[1], argv[2], argv[3], err, sizeof err);
    if (rc != 0) {
        fprintf(stderr, "[ERROR] nxpatch_apply failed (%d): %s\n", rc, err);
        return 1;
    }
    printf("[OK] applied %s -> %s (Build ID + raw/base/output SHA-256 verified)\n", argv[2], argv[3]);
    return 0;
}
#endif
