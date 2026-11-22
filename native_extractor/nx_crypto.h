#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- AES-128 ---- */
typedef struct {
    uint32_t ek[44]; /* encryption round keys */
    uint32_t dk[44]; /* decryption round keys */
} nx_aes128_ctx;

void nx_aes128_init(nx_aes128_ctx *ctx, const uint8_t key[16]);
void nx_aes128_ecb_encrypt(const nx_aes128_ctx *ctx, const uint8_t in[16], uint8_t out[16]);
void nx_aes128_ecb_decrypt(const nx_aes128_ctx *ctx, const uint8_t in[16], uint8_t out[16]);

/* AES-128-CTR with 16-byte big-endian counter (increments low 64-bit block counter).
 * Handles arbitrary byte offsets within the CTR stream (offset = byte position from counter base). */
void nx_aes128_ctr_xor(const nx_aes128_ctx *ctx, const uint8_t nonce_be[8], uint64_t byte_offset,
                       uint8_t *buf, size_t len);

/* Nintendo Switch NCA3 AES-128-XTS (32-byte key: key[0..15]=data, key[16..31]=tweak, big-endian sector_index). */
void nx_aes128_xts_decrypt_sector(const uint8_t key32[32], uint64_t sector_index,
                                  const uint8_t *in, uint8_t *out, size_t sector_size);

/* ---- SHA-256 ---- */
typedef struct {
    uint32_t s[8];
    uint64_t bitlen;
    uint8_t buf[64];
    size_t buflen;
} nx_sha256_ctx;

void nx_sha256_init(nx_sha256_ctx *c);
void nx_sha256_update(nx_sha256_ctx *c, const uint8_t *data, size_t len);
void nx_sha256_final(nx_sha256_ctx *c, uint8_t out[32]);
void nx_sha256_buf(const uint8_t *data, size_t len, uint8_t out[32]);

/* ---- Nintendo Switch Keyset (loaded from prod.keys fd; values never logged) ---- */
#define NX_MAX_KEYGEN 32

typedef struct {
    int has_header_key;
    uint8_t header_key[32];

    uint8_t has_titlekek[NX_MAX_KEYGEN];
    uint8_t titlekek[NX_MAX_KEYGEN][16];

    uint8_t has_kak_app[NX_MAX_KEYGEN];
    uint8_t kak_app[NX_MAX_KEYGEN][16];

    uint8_t has_kak_ocean[NX_MAX_KEYGEN];
    uint8_t kak_ocean[NX_MAX_KEYGEN][16];

    uint8_t has_kak_sys[NX_MAX_KEYGEN];
    uint8_t kak_sys[NX_MAX_KEYGEN][16];
} nx_keyset;

/* Parse prod.keys directly from raw fd using pread (offset 0). Returns 0 on success. */
int nx_keyset_load_fd(int keys_fd, nx_keyset *ks, char *err, size_t errcap);

#ifdef __cplusplus
}
#endif
