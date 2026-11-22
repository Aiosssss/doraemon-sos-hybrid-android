#define _FILE_OFFSET_BITS 64
#include "nx_crypto.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---------------- FIPS-197 AES-128 ---------------- */
static const uint8_t SBOX[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};

static const uint8_t RSBOX[256] = {
    0x52,0x09,0x6a,0xd5,0x30,0x36,0xa5,0x38,0xbf,0x40,0xa3,0x9e,0x81,0xf3,0xd7,0xfb,
    0x7c,0xe3,0x39,0x82,0x9b,0x2f,0xff,0x87,0x34,0x8e,0x43,0x44,0xc4,0xde,0xe9,0xcb,
    0x54,0x7b,0x94,0x32,0xa6,0xc2,0x23,0x3d,0xee,0x4c,0x95,0x0b,0x42,0xfa,0xc3,0x4e,
    0x08,0x2e,0xa1,0x66,0x28,0xd9,0x24,0xb2,0x76,0x5b,0xa2,0x49,0x6d,0x8b,0xd1,0x25,
    0x72,0xf8,0xf6,0x64,0x86,0x68,0x98,0x16,0xd4,0xa4,0x5c,0xcc,0x5d,0x65,0xb6,0x92,
    0x6c,0x70,0x48,0x50,0xfd,0xed,0xb9,0xda,0x5e,0x15,0x46,0x57,0xa7,0x8d,0x9d,0x84,
    0x90,0xd8,0xab,0x00,0x8c,0xbc,0xd3,0x0a,0xf7,0xe4,0x58,0x05,0xb8,0xb3,0x45,0x06,
    0xd0,0x2c,0x1e,0x8f,0xca,0x3f,0x0f,0x02,0xc1,0xaf,0xbd,0x03,0x01,0x13,0x8a,0x6b,
    0x3a,0x91,0x11,0x41,0x4f,0x67,0xdc,0xea,0x97,0xf2,0xcf,0xce,0xf0,0xb4,0xe6,0x73,
    0x96,0xac,0x74,0x22,0xe7,0xad,0x35,0x85,0xe2,0xf9,0x37,0xe8,0x1c,0x75,0xdf,0x6e,
    0x47,0xf1,0x1a,0x71,0x1d,0x29,0xc5,0x89,0x6f,0xb7,0x62,0x0e,0xaa,0x18,0xbe,0x1b,
    0xfc,0x56,0x3e,0x4b,0xc6,0xd2,0x79,0x20,0x9a,0xdb,0xc0,0xfe,0x78,0xcd,0x5a,0xf4,
    0x1f,0xdd,0xa8,0x33,0x88,0x07,0xc7,0x31,0xb1,0x12,0x10,0x59,0x27,0x80,0xec,0x5f,
    0x60,0x51,0x7f,0xa9,0x19,0xb5,0x4a,0x0d,0x2d,0xe5,0x7a,0x9f,0x93,0xc9,0x9c,0xef,
    0xa0,0xe0,0x3b,0x4d,0xae,0x2a,0xf5,0xb0,0xc8,0xeb,0xbb,0x3c,0x83,0x53,0x99,0x61,
    0x17,0x2b,0x04,0x7e,0xba,0x77,0xd6,0x26,0xe1,0x69,0x14,0x63,0x55,0x21,0x0c,0x7d
};

static const uint8_t RCON[10] = { 0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36 };

static inline uint8_t xtime(uint8_t x) { return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1b : 0x00)); }
static inline uint8_t gmul(uint8_t a, uint8_t b) {
    uint8_t p = 0;
    for (int i = 0; i < 8; i++) {
        if (b & 1) p ^= a;
        a = xtime(a);
        b >>= 1;
    }
    return p;
}

static uint32_t sub_word(uint32_t w) {
    return ((uint32_t)SBOX[(w >> 24) & 0xff] << 24) |
           ((uint32_t)SBOX[(w >> 16) & 0xff] << 16) |
           ((uint32_t)SBOX[(w >>  8) & 0xff] <<  8) |
           ((uint32_t)SBOX[w & 0xff]);
}

static inline uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static inline void wr_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

/* Precomputed T-tables for fast AES-128 encryption (critical for streaming 2.24 GiB RomFS!) */
static uint32_t TE0[256], TE1[256], TE2[256], TE3[256];
static int g_aes_tables_ready = 0;

static void aes_init_tables(void) {
    if (g_aes_tables_ready) return;
    for (int i = 0; i < 256; i++) {
        uint8_t s = SBOX[i];
        uint8_t s2 = xtime(s);
        uint8_t s3 = (uint8_t)(s2 ^ s);
        uint32_t t0 = ((uint32_t)s2 << 24) | ((uint32_t)s << 16) | ((uint32_t)s << 8) | s3;
        TE0[i] = t0;
        TE1[i] = (t0 >> 8) | (t0 << 24);
        TE2[i] = (t0 >> 16) | (t0 << 16);
        TE3[i] = (t0 >> 24) | (t0 << 8);
    }
    g_aes_tables_ready = 1;
}

void nx_aes128_init(nx_aes128_ctx *ctx, const uint8_t key[16]) {
    aes_init_tables();
    for (int i = 0; i < 4; i++) ctx->ek[i] = be32(key + i * 4);
    for (int i = 4; i < 44; i++) {
        uint32_t tmp = ctx->ek[i - 1];
        if ((i & 3) == 0) {
            tmp = ((tmp << 8) | (tmp >> 24));
            tmp = sub_word(tmp) ^ ((uint32_t)RCON[i / 4 - 1] << 24);
        }
        ctx->ek[i] = ctx->ek[i - 4] ^ tmp;
    }
    for (int i = 0; i < 44; i++) ctx->dk[i] = ctx->ek[i];
}

void nx_aes128_ecb_encrypt(const nx_aes128_ctx *ctx, const uint8_t in[16], uint8_t out[16]) {
    const uint32_t *rk = ctx->ek;
    uint32_t s0 = be32(in + 0) ^ rk[0];
    uint32_t s1 = be32(in + 4) ^ rk[1];
    uint32_t s2 = be32(in + 8) ^ rk[2];
    uint32_t s3 = be32(in + 12) ^ rk[3];
    uint32_t t0, t1, t2, t3;
    for (int r = 1; r < 10; r++) {
        rk += 4;
        t0 = TE0[(s0 >> 24)] ^ TE1[(s1 >> 16) & 0xff] ^ TE2[(s2 >> 8) & 0xff] ^ TE3[s3 & 0xff] ^ rk[0];
        t1 = TE0[(s1 >> 24)] ^ TE1[(s2 >> 16) & 0xff] ^ TE2[(s3 >> 8) & 0xff] ^ TE3[s0 & 0xff] ^ rk[1];
        t2 = TE0[(s2 >> 24)] ^ TE1[(s3 >> 16) & 0xff] ^ TE2[(s0 >> 8) & 0xff] ^ TE3[s1 & 0xff] ^ rk[2];
        t3 = TE0[(s3 >> 24)] ^ TE1[(s0 >> 16) & 0xff] ^ TE2[(s1 >> 8) & 0xff] ^ TE3[s2 & 0xff] ^ rk[3];
        s0 = t0; s1 = t1; s2 = t2; s3 = t3;
    }
    rk += 4;
    t0 = ((uint32_t)SBOX[s0 >> 24] << 24) | ((uint32_t)SBOX[(s1 >> 16) & 0xff] << 16) |
         ((uint32_t)SBOX[(s2 >> 8) & 0xff] << 8) | SBOX[s3 & 0xff];
    t1 = ((uint32_t)SBOX[s1 >> 24] << 24) | ((uint32_t)SBOX[(s2 >> 16) & 0xff] << 16) |
         ((uint32_t)SBOX[(s3 >> 8) & 0xff] << 8) | SBOX[s0 & 0xff];
    t2 = ((uint32_t)SBOX[s2 >> 24] << 24) | ((uint32_t)SBOX[(s3 >> 16) & 0xff] << 16) |
         ((uint32_t)SBOX[(s0 >> 8) & 0xff] << 8) | SBOX[s1 & 0xff];
    t3 = ((uint32_t)SBOX[s3 >> 24] << 24) | ((uint32_t)SBOX[(s0 >> 16) & 0xff] << 16) |
         ((uint32_t)SBOX[(s1 >> 8) & 0xff] << 8) | SBOX[s2 & 0xff];
    wr_be32(out + 0,  t0 ^ rk[0]);
    wr_be32(out + 4,  t1 ^ rk[1]);
    wr_be32(out + 8,  t2 ^ rk[2]);
    wr_be32(out + 12, t3 ^ rk[3]);
}

static void add_round_key_bytes(uint8_t st[16], const uint32_t rk[4]) {
    for (int c = 0; c < 4; c++) {
        uint32_t w = rk[c];
        st[c*4 + 0] ^= (uint8_t)(w >> 24);
        st[c*4 + 1] ^= (uint8_t)(w >> 16);
        st[c*4 + 2] ^= (uint8_t)(w >> 8);
        st[c*4 + 3] ^= (uint8_t)w;
    }
}

void nx_aes128_ecb_decrypt(const nx_aes128_ctx *ctx, const uint8_t in[16], uint8_t out[16]) {
    uint8_t st[16];
    memcpy(st, in, 16);
    add_round_key_bytes(st, ctx->dk + 40);
    for (int r = 9; r >= 0; r--) {
        /* InvShiftRows */
        uint8_t t;
        t = st[13]; st[13] = st[9];  st[9]  = st[5];  st[5]  = st[1];  st[1]  = t;
        t = st[2];  st[2]  = st[10]; st[10] = t;
        t = st[6];  st[6]  = st[14]; st[14] = t;
        t = st[3];  st[3]  = st[7];  st[7]  = st[11]; st[11] = st[15]; st[15] = t;
        /* InvSubBytes */
        for (int i = 0; i < 16; i++) st[i] = RSBOX[st[i]];
        add_round_key_bytes(st, ctx->dk + r * 4);
        if (r > 0) {
            /* InvMixColumns */
            for (int c = 0; c < 4; c++) {
                uint8_t *col = st + c * 4;
                uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
                col[0] = (uint8_t)(gmul(a0, 0x0e) ^ gmul(a1, 0x0b) ^ gmul(a2, 0x0d) ^ gmul(a3, 0x09));
                col[1] = (uint8_t)(gmul(a0, 0x09) ^ gmul(a1, 0x0e) ^ gmul(a2, 0x0b) ^ gmul(a3, 0x0d));
                col[2] = (uint8_t)(gmul(a0, 0x0d) ^ gmul(a1, 0x09) ^ gmul(a2, 0x0e) ^ gmul(a3, 0x0b));
                col[3] = (uint8_t)(gmul(a0, 0x0b) ^ gmul(a1, 0x0d) ^ gmul(a2, 0x09) ^ gmul(a3, 0x0e));
            }
        }
    }
    memcpy(out, st, 16);
}

void nx_aes128_ctr_xor(const nx_aes128_ctx *ctx, const uint8_t nonce_be[8], uint64_t byte_offset,
                       uint8_t *buf, size_t len) {
    uint64_t blk = byte_offset >> 4;
    size_t skip = (size_t)(byte_offset & 15u);
    uint8_t ctr[16], ks[16];
    memcpy(ctr, nonce_be, 8);
    size_t pos = 0;
    while (pos < len) {
        for (int i = 0; i < 8; i++) ctr[8 + i] = (uint8_t)(blk >> (56 - i * 8));
        nx_aes128_ecb_encrypt(ctx, ctr, ks);
        size_t take = 16 - skip;
        if (take > len - pos) take = len - pos;
        for (size_t i = 0; i < take; i++) buf[pos + i] ^= ks[skip + i];
        pos += take;
        skip = 0;
        blk++;
    }
}

void nx_aes128_xts_decrypt_sector(const uint8_t key32[32], uint64_t sector_index,
                                  const uint8_t *in, uint8_t *out, size_t sector_size) {
    nx_aes128_ctx data_ctx, tweak_ctx;
    nx_aes128_init(&data_ctx, key32);
    nx_aes128_init(&tweak_ctx, key32 + 16);

    /* Nintendo Switch XTS tweak: 16-byte big-endian sector index (upper 8 bytes 0, lower 8 bytes BE sector_index) */
    uint8_t tweak[16];
    memset(tweak, 0, 8);
    for (int i = 0; i < 8; i++) tweak[8 + i] = (uint8_t)(sector_index >> (56 - i * 8));
    nx_aes128_ecb_encrypt(&tweak_ctx, tweak, tweak);

    for (size_t off = 0; off + 16 <= sector_size; off += 16) {
        uint8_t tmp[16];
        for (int i = 0; i < 16; i++) tmp[i] = in[off + i] ^ tweak[i];
        nx_aes128_ecb_decrypt(&data_ctx, tmp, tmp);
        for (int i = 0; i < 16; i++) out[off + i] = tmp[i] ^ tweak[i];
        /* GF(2^128) multiply tweak by alpha (x) in little-endian byte order */
        uint8_t carry = 0;
        for (int i = 0; i < 16; i++) {
            uint8_t next_carry = (tweak[i] >> 7) & 1;
            tweak[i] = (uint8_t)((tweak[i] << 1) | carry);
            carry = next_carry;
        }
        if (carry) tweak[0] ^= 0x87;
    }
}

/* ---------------- FIPS 180-4 SHA-256 ---------------- */
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

static void sha256_block(nx_sha256_ctx *c, const uint8_t b[64]) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) w[i] = be32(b + i * 4);
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr32(w[i-15], 7) ^ rotr32(w[i-15], 18) ^ (w[i-15] >> 3);
        uint32_t s1 = rotr32(w[i-2], 17) ^ rotr32(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint32_t a=c->s[0], b0=c->s[1], cc=c->s[2], d=c->s[3], e=c->s[4], f=c->s[5], g=c->s[6], h=c->s[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = h + S1 + ch + K256[i] + w[i];
        uint32_t S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        uint32_t maj = (a & b0) ^ (a & cc) ^ (b0 & cc);
        uint32_t t2 = S0 + maj;
        h = g; g = f; f = e; e = d + t1; d = cc; cc = b0; b0 = a; a = t1 + t2;
    }
    c->s[0]+=a; c->s[1]+=b0; c->s[2]+=cc; c->s[3]+=d;
    c->s[4]+=e; c->s[5]+=f;  c->s[6]+=g;  c->s[7]+=h;
}

void nx_sha256_init(nx_sha256_ctx *c) {
    c->s[0]=0x6a09e667; c->s[1]=0xbb67ae85; c->s[2]=0x3c6ef372; c->s[3]=0xa54ff53a;
    c->s[4]=0x510e527f; c->s[5]=0x9b05688c; c->s[6]=0x1f83d9ab; c->s[7]=0x5be0cd19;
    c->bitlen = 0; c->buflen = 0;
}

void nx_sha256_update(nx_sha256_ctx *c, const uint8_t *data, size_t len) {
    c->bitlen += (uint64_t)len * 8;
    while (len > 0) {
        if (c->buflen == 0 && len >= 64) {
            sha256_block(c, data);
            data += 64; len -= 64;
            continue;
        }
        size_t take = 64 - c->buflen;
        if (take > len) take = len;
        memcpy(c->buf + c->buflen, data, take);
        c->buflen += take; data += take; len -= take;
        if (c->buflen == 64) { sha256_block(c, c->buf); c->buflen = 0; }
    }
}

void nx_sha256_final(nx_sha256_ctx *c, uint8_t out[32]) {
    uint64_t bl = c->bitlen;
    uint8_t pad = 0x80;
    nx_sha256_update(c, &pad, 1);
    uint8_t z = 0;
    while (c->buflen != 56) nx_sha256_update(c, &z, 1);
    uint8_t lb[8];
    for (int i = 0; i < 8; i++) lb[i] = (uint8_t)(bl >> (56 - i * 8));
    nx_sha256_update(c, lb, 8);
    for (int i = 0; i < 8; i++) wr_be32(out + i * 4, c->s[i]);
}

void nx_sha256_buf(const uint8_t *data, size_t len, uint8_t out[32]) {
    nx_sha256_ctx c;
    nx_sha256_init(&c);
    nx_sha256_update(&c, data, len);
    nx_sha256_final(&c, out);
}

/* ---------------- prod.keys Parser (fd-based) ---------------- */
static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int parse_hex(const char *s, uint8_t *out, size_t expected_bytes) {
    size_t n = strlen(s);
    if (n != expected_bytes * 2) return -1;
    for (size_t i = 0; i < expected_bytes; i++) {
        int hi = hex_val(s[i * 2]), lo = hex_val(s[i * 2 + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return 0;
}

static int parse_idx_suffix(const char *name, const char *prefix) {
    size_t plen = strlen(prefix);
    if (strncmp(name, prefix, plen) != 0) return -1;
    const char *p = name + plen;
    if (strlen(p) != 2) return -1;
    int hi = hex_val(p[0]), lo = hex_val(p[1]);
    if (hi < 0 || lo < 0) return -1;
    int idx = (hi << 4) | lo;
    return (idx >= 0 && idx < NX_MAX_KEYGEN) ? idx : -1;
}

int nx_keyset_load_fd(int keys_fd, nx_keyset *ks, char *err, size_t errcap) {
    memset(ks, 0, sizeof *ks);
    if (keys_fd < 0) { snprintf(err, errcap, "invalid prod.keys file descriptor"); return -1; }

    /* Read prod.keys up to 512 KiB via pread (never modifies fd offset) */
    enum { MAX_KEYS_SZ = 512 * 1024 };
    char *buf = malloc(MAX_KEYS_SZ + 1);
    if (!buf) { snprintf(err, errcap, "OOM reading prod.keys"); return -2; }
    size_t total = 0;
    while (total < MAX_KEYS_SZ) {
        ssize_t r = pread(keys_fd, buf + total, MAX_KEYS_SZ - total, (off_t)total);
        if (r < 0) { free(buf); snprintf(err, errcap, "pread failed on prod.keys"); return -3; }
        if (r == 0) break;
        total += (size_t)r;
    }
    buf[total] = '\0';

    char *p = buf;
    while (*p) {
        char *eol = strchr(p, '\n');
        if (eol) *eol = '\0';
        char *eq = strchr(p, '=');
        if (eq) {
            *eq = '\0';
            char *k = p, *v = eq + 1;
            while (*k && isspace((unsigned char)*k)) k++;
            char *ke = k + strlen(k);
            while (ke > k && isspace((unsigned char)ke[-1])) *--ke = '\0';
            while (*v && isspace((unsigned char)*v)) v++;
            char *ve = v + strlen(v);
            while (ve > v && (isspace((unsigned char)ve[-1]) || ve[-1] == '\r')) *--ve = '\0';

            if (*k && *k != '#' && *k != ';') {
                int idx;
                if (strcmp(k, "header_key") == 0 && parse_hex(v, ks->header_key, 32) == 0) {
                    ks->has_header_key = 1;
                } else if ((idx = parse_idx_suffix(k, "titlekek_")) >= 0 && parse_hex(v, ks->titlekek[idx], 16) == 0) {
                    ks->has_titlekek[idx] = 1;
                } else if ((idx = parse_idx_suffix(k, "key_area_key_application_")) >= 0 && parse_hex(v, ks->kak_app[idx], 16) == 0) {
                    ks->has_kak_app[idx] = 1;
                } else if ((idx = parse_idx_suffix(k, "key_area_key_ocean_")) >= 0 && parse_hex(v, ks->kak_ocean[idx], 16) == 0) {
                    ks->has_kak_ocean[idx] = 1;
                } else if ((idx = parse_idx_suffix(k, "key_area_key_system_")) >= 0 && parse_hex(v, ks->kak_sys[idx], 16) == 0) {
                    ks->has_kak_sys[idx] = 1;
                }
            }
        }
        if (!eol) break;
        p = eol + 1;
    }
    free(buf);

    if (!ks->has_header_key) {
        snprintf(err, errcap, "prod.keys is missing required key: header_key");
        return -4;
    }
    return 0;
}
