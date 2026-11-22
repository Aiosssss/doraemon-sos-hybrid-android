#pragma once
#include "nx_crypto.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char name[128];
    uint64_t offset; /* absolute offset in NSP fd */
    uint64_t size;
} nx_pfs0_entry;

typedef struct {
    int fd;
    uint32_t count;
    nx_pfs0_entry *entries;
} nx_nsp;

typedef struct {
    uint64_t title_id;
    uint32_t version;
    uint8_t meta_type;   /* 0x80=Application, 0x81=Patch, 0x82=AddOnContent */
    uint64_t app_id;     /* ApplicationId for Patch/AOC, or title_id for Application */
} nx_cnmt_info;

typedef struct {
    uint64_t media_start; /* in bytes from NCA start */
    uint64_t media_end;
    uint8_t fs_type;      /* 0=RomFS, 1=PFS0 */
    uint8_t hash_type;    /* 2=PFS0 Sha256, 3=IVFC */
    uint8_t enc_type;     /* 1=None, 3=AesCtr, 4=AesCtrEx (BKTR) */
    uint8_t section_ctr[8]; /* raw 8 bytes from FsHeader + 0x140 */
    uint64_t data_offset; /* relative to section start: PFS0 layer1 or IVFC level5 offset */
    uint64_t data_size;   /* PFS0 layer1 or IVFC level5 size */
    /* BKTR patch_info (valid if enc_type == 4) */
    uint64_t reloc_off;
    uint64_t reloc_sz;
    uint64_t enc_off;
    uint64_t enc_sz;
    int exists;
} nx_nca_section;

typedef struct {
    int fd;
    uint64_t nca_offset;  /* offset of NCA inside NSP fd */
    uint64_t nca_size;
    uint8_t content_type; /* 0=Program, 1=Meta, 2=Control, 3=Manual, 4=Data, 5=PublicData */
    uint64_t title_id;
    uint8_t keygen_idx;   /* 0..31 index into prod.keys */
    uint8_t rights_id[16];
    int has_rights_id;
    uint8_t body_key[16];
    nx_aes128_ctx body_aes;
    nx_nca_section sec[4];
} nx_nca;

typedef struct {
    uint64_t virt_offset;
    uint64_t phys_offset;
    uint32_t is_patch;
} nx_bktr_reloc_entry;

typedef struct {
    uint64_t offset;
    uint32_t ctr_val;
} nx_bktr_enc_entry;

typedef struct {
    const nx_nca *base_nca;
    int base_sec;
    const nx_nca *upd_nca;
    int upd_sec;
    uint64_t total_size;
    nx_bktr_reloc_entry *reloc;
    uint32_t reloc_count;
    nx_bktr_enc_entry *enc;
    uint32_t enc_count;
} nx_bktr;

/* Progress & cancellation callback for streaming RomFS extraction */
typedef void (*nx_progress_fn)(void *user, const char *stage, uint64_t files_done,
                               uint64_t files_total, uint64_t bytes_done, uint64_t bytes_total);

/* Open/close NSP (PFS0) from raw fd */
int nx_nsp_open(int fd, nx_nsp *nsp, char *err, size_t errcap);
void nx_nsp_close(nx_nsp *nsp);

/* Open a specific NCA inside an NSP (resolves titlekey from .tik or key_area from prod.keys) */
int nx_nca_open(const nx_nsp *nsp, const nx_pfs0_entry *ent, const nx_keyset *ks,
                nx_nca *nca, char *err, size_t errcap);

/* Find and parse CNMT metadata from an NSP's Meta NCA (.cnmt.nca) */
int nx_nsp_read_cnmt(const nx_nsp *nsp, const nx_keyset *ks, nx_cnmt_info *info, char *err, size_t errcap);

/* Pick an NCA of given content_type (and optional title_id if != 0) inside an NSP */
int nx_nsp_open_nca_by_type(const nx_nsp *nsp, const nx_keyset *ks, uint8_t content_type,
                            uint64_t match_tid, nx_nca *nca, char *err, size_t errcap);

/* Extract PFS0 section (e.g. ExeFS section 0) into out_dir */
int nx_nca_extract_pfs0(const nx_nca *nca, int sec_idx, const char *out_dir,
                        volatile int *cancel_flag, char *err, size_t errcap);

/* Extract plain RomFS section (e.g. Base-only or AOC DLC) into out_dir */
int nx_nca_extract_romfs(const nx_nca *nca, int sec_idx, const char *out_dir,
                         nx_progress_fn cb, void *cb_user, volatile int *cancel_flag,
                         uint64_t *out_files, uint64_t *out_bytes, char *err, size_t errcap);

/* Open BKTR overlay (Base Program NCA + Update Program NCA) and stream merged RomFS into out_dir */
int nx_bktr_open(const nx_nca *base_nca, int base_sec, const nx_nca *upd_nca, int upd_sec,
                 nx_bktr *bktr, char *err, size_t errcap);
void nx_bktr_close(nx_bktr *bktr);

int nx_bktr_extract_romfs(const nx_bktr *bktr, const char *out_dir,
                          nx_progress_fn cb, void *cb_user, volatile int *cancel_flag,
                          uint64_t *out_files, uint64_t *out_bytes, char *err, size_t errcap);

#ifdef __cplusplus
}
#endif
