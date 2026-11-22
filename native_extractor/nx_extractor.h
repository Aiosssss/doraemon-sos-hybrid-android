#pragma once
#include "nx_nsp.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NX_EXPECTED_BASE_TID   0x01008EE0177A8000ULL
#define NX_EXPECTED_UPD_TID    0x01008EE0177A8800ULL
#define NX_EXPECTED_UPD_VER    262144U /* Update 1.1.2 */
#define NX_DEFAULT_MIN_FREE    (3221225472ULL) /* 3.0 GiB safe margin */

typedef struct {
    int keys_fd;
    int base_fd;
    int update_fd;
    const int *dlc_fds;
    int dlc_count;
    const char *patches_dir;   /* folder holding {rtld,main,subsdk0,sdk}.{nxpatch,fixups.tsv,gates.tsv} */
    const char *dest_root;     /* parent dir (Context.filesDir); stages into dest_root/extract_tmp -> commits to dest_root/game_data */
    uint64_t min_free_bytes;   /* 0 = NX_DEFAULT_MIN_FREE */
    nx_progress_fn progress_cb;
    void *progress_user;
    volatile int *cancel_flag;
} nx_extract_params;

typedef struct {
    uint64_t base_title_id;
    uint64_t update_title_id;
    uint32_t update_version;
    uint64_t romfs_files;
    uint64_t romfs_bytes;
    int aoc_count;
    int aoc_indices[32];
    char main_build_id[33];
    char subsdk0_build_id[33];
    char sdk_build_id[33];
    char rtld_build_id[33];
} nx_extract_result;

/*
 * Run full verified Hybrid extraction & NSO patching into dest_root/extract_tmp,
 * then atomically commit to dest_root/game_data with .ready.json.
 * On any error or cancellation, extract_tmp is deleted and existing game_data is untouched.
 * Returns 0 on success; negative error code + error message on failure.
 */
int nx_extract_hybrid_game(const nx_extract_params *p, nx_extract_result *res,
                           char *err, size_t errcap);

/* Query free bytes on filesystem hosting path (returns 0 on failure). */
uint64_t nx_query_free_space(const char *path);

#ifdef __cplusplus
}
#endif
