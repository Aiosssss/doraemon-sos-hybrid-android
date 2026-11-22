#define _FILE_OFFSET_BITS 64
#include "nx_extractor.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

typedef struct {
    volatile int *cancel_after_flag;
    uint64_t cancel_at_files;
    uint64_t callbacks;
} prog_ctx;

static void on_progress(void *user, const char *stage, uint64_t fdone, uint64_t ftotal,
                        uint64_t bdone, uint64_t btotal) {
    prog_ctx *c = (prog_ctx *)user;
    c->callbacks++;
    if (c->cancel_after_flag && strcmp(stage, "romfs") == 0 && fdone >= c->cancel_at_files) {
        *c->cancel_after_flag = 1;
    }
    if (strcmp(stage, "romfs") != 0 || (fdone % 2048 == 0) || fdone == ftotal) {
        double pct = btotal ? (100.0 * (double)bdone / (double)btotal) : 0.0;
        printf("  [progress] stage=%-14s files=%llu/%llu bytes=%llu/%llu (%.1f%%)\n",
               stage, (unsigned long long)fdone, (unsigned long long)ftotal,
               (unsigned long long)bdone, (unsigned long long)btotal, pct);
        fflush(stdout);
    }
}

static int run_negative_suite(int keys_fd, int base_fd, int upd_fd, const int *dlc_fds,
                              int dlc_count, const char *patches_dir, const char *dest_root) {
    printf("\n=== RUNNING NEGATIVE TEST SUITE ===\n");
    char err[512];
    nx_extract_result res;
    int passed = 0, total = 5;

    /* Neg 1: Bad / incomplete prod.keys */
    {
        int bad_k = open("/tmp/nx_bad_keys.txt", O_RDWR | O_CREAT | O_TRUNC, 0600);
        const char *junk = "header_key = 0000000000000000000000000000000000000000000000000000000000000000\n";
        if (write(bad_k, junk, strlen(junk)) < 0) {}
        nx_extract_params p = {
            .keys_fd = bad_k, .base_fd = base_fd, .update_fd = upd_fd,
            .dlc_fds = dlc_fds, .dlc_count = dlc_count,
            .patches_dir = patches_dir, .dest_root = dest_root, .min_free_bytes = 1024
        };
        int rc = nx_extract_hybrid_game(&p, &res, err, sizeof err);
        close(bad_k); unlink("/tmp/nx_bad_keys.txt");
        if (rc != 0) { printf("  [PASS 1/5] bad prod.keys rejected (rc=%d): %s\n", rc, err); passed++; }
        else { printf("  [FAIL 1/5] bad prod.keys unexpectedly succeeded!\n"); }
    }

    /* Neg 2: Wrong Update NSP (pass Base NSP as Update -> CNMT type/version mismatch) */
    {
        nx_extract_params p = {
            .keys_fd = keys_fd, .base_fd = base_fd, .update_fd = base_fd,
            .dlc_fds = dlc_fds, .dlc_count = dlc_count,
            .patches_dir = patches_dir, .dest_root = dest_root, .min_free_bytes = 1024
        };
        int rc = nx_extract_hybrid_game(&p, &res, err, sizeof err);
        if (rc != 0) { printf("  [PASS 2/5] wrong Update NSP rejected (rc=%d): %s\n", rc, err); passed++; }
        else { printf("  [FAIL 2/5] wrong Update NSP unexpectedly succeeded!\n"); }
    }

    /* Neg 3: Duplicate DLC + Corrupt DLC */
    {
        int dup_dlcs[2] = { dlc_fds[0], dlc_fds[0] };
        nx_extract_params p = {
            .keys_fd = keys_fd, .base_fd = base_fd, .update_fd = upd_fd,
            .dlc_fds = dup_dlcs, .dlc_count = 2,
            .patches_dir = patches_dir, .dest_root = dest_root, .min_free_bytes = 1024
        };
        int rc1 = nx_extract_hybrid_game(&p, &res, err, sizeof err);
        char e1[256]; snprintf(e1, sizeof e1, "%s", err);

        int bad_dlc = open("/tmp/nx_corrupt_dlc.nsp", O_RDWR | O_CREAT | O_TRUNC, 0600);
        const char *corrupt = "NOT_A_PFS0_CONTAINER";
        if (write(bad_dlc, corrupt, strlen(corrupt)) < 0) {}
        int bad_list[1] = { bad_dlc };
        p.dlc_fds = bad_list; p.dlc_count = 1;
        int rc2 = nx_extract_hybrid_game(&p, &res, err, sizeof err);
        close(bad_dlc); unlink("/tmp/nx_corrupt_dlc.nsp");
        if (rc1 != 0 && rc2 != 0) {
            printf("  [PASS 3/5] duplicate DLC (rc=%d: %s) & corrupt DLC (rc=%d: %s) rejected\n", rc1, e1, rc2, err);
            passed++;
        } else {
            printf("  [FAIL 3/5] duplicate/corrupt DLC test failed (rc1=%d rc2=%d)\n", rc1, rc2);
        }
    }

    /* Neg 4: Insufficient disk space simulation (require 999 TiB) */
    {
        nx_extract_params p = {
            .keys_fd = keys_fd, .base_fd = base_fd, .update_fd = upd_fd,
            .dlc_fds = dlc_fds, .dlc_count = dlc_count,
            .patches_dir = patches_dir, .dest_root = dest_root,
            .min_free_bytes = 999ULL * 1024ULL * 1024ULL * 1024ULL * 1024ULL
        };
        int rc = nx_extract_hybrid_game(&p, &res, err, sizeof err);
        if (rc == -3) { printf("  [PASS 4/5] insufficient space rejected (rc=%d): %s\n", rc, err); passed++; }
        else { printf("  [FAIL 4/5] insufficient space check failed (rc=%d)\n", rc); }
    }

    /* Neg 5: Mid-extraction cancellation -> verify extract_tmp cleaned AND existing game_data/.ready.json untouched! */
    {
        volatile int cancel = 0;
        prog_ctx pctx = { .cancel_after_flag = &cancel, .cancel_at_files = 128, .callbacks = 0 };
        char ready_p[1024], tmp_p[1024];
        snprintf(ready_p, sizeof ready_p, "%s/game_data/.ready.json", dest_root);
        snprintf(tmp_p,   sizeof tmp_p,   "%s/extract_tmp", dest_root);
        struct stat st_before, st_after;
        int had_ready = (stat(ready_p, &st_before) == 0);

        nx_extract_params p = {
            .keys_fd = keys_fd, .base_fd = base_fd, .update_fd = upd_fd,
            .dlc_fds = dlc_fds, .dlc_count = dlc_count,
            .patches_dir = patches_dir, .dest_root = dest_root,
            .min_free_bytes = 1024,
            .progress_cb = on_progress, .progress_user = &pctx,
            .cancel_flag = &cancel
        };
        int rc = nx_extract_hybrid_game(&p, &res, err, sizeof err);
        int tmp_gone = (access(tmp_p, F_OK) != 0);
        int ready_intact = had_ready && (stat(ready_p, &st_after) == 0) && (st_before.st_ino == st_after.st_ino);
        if (rc == -99 && tmp_gone && ready_intact) {
            printf("  [PASS 5/5] mid-extraction cancel (rc=%d): extract_tmp cleaned & existing game_data preserved (inode=%llu)\n",
                   rc, (unsigned long long)st_after.st_ino);
            passed++;
        } else {
            printf("  [FAIL 5/5] cancel check failed (rc=%d tmp_gone=%d ready_intact=%d err=%s)\n",
                   rc, tmp_gone, ready_intact, err);
        }
    }

    printf("NEGATIVE SUITE RESULT: %d / %d PASSED\n", passed, total);
    return passed == total ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc < 6) {
        fprintf(stderr, "usage: nx_host_test <prod.keys> <base.nsp> <update.nsp> <patches_dir> <dest_root> [dlc1.nsp ...]\n");
        return 2;
    }
    int keys_fd = open(argv[1], O_RDONLY);
    int base_fd = open(argv[2], O_RDONLY);
    int upd_fd  = open(argv[3], O_RDONLY);
    const char *patches_dir = argv[4];
    const char *dest_root   = argv[5];
    int dlc_count = argc - 6;
    int dlc_fds[32];
    for (int i = 0; i < dlc_count && i < 32; i++) {
        dlc_fds[i] = open(argv[6 + i], O_RDONLY);
        if (dlc_fds[i] < 0) { perror("open dlc"); return 2; }
    }
    if (keys_fd < 0 || base_fd < 0 || upd_fd < 0) { perror("open input"); return 2; }

    volatile int cancel = 0;
    prog_ctx pctx = { .cancel_after_flag = NULL, .cancel_at_files = 0, .callbacks = 0 };
    nx_extract_params p = {
        .keys_fd = keys_fd, .base_fd = base_fd, .update_fd = upd_fd,
        .dlc_fds = dlc_fds, .dlc_count = dlc_count,
        .patches_dir = patches_dir, .dest_root = dest_root,
        .min_free_bytes = NX_DEFAULT_MIN_FREE,
        .progress_cb = on_progress, .progress_user = &pctx,
        .cancel_flag = &cancel
    };

    printf("=== STARTING FULL FD-BASED HYBRID EXTRACTION ===\n");
    double t0 = now_sec();
    nx_extract_result res;
    char err[512] = "";
    int rc = nx_extract_hybrid_game(&p, &res, err, sizeof err);
    double dt = now_sec() - t0;

    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    double peak_mib = (double)ru.ru_maxrss / 1024.0;

    if (rc != 0) {
        fprintf(stderr, "[ERROR] extraction failed (rc=%d): %s\n", rc, err);
        return 1;
    }
    printf("=== EXTRACTION SUCCEEDED in %.2f s (Peak RSS: %.2f MiB, callbacks=%llu) ===\n",
           dt, peak_mib, (unsigned long long)pctx.callbacks);
    printf("  Base TID:   %016llx\n", (unsigned long long)res.base_title_id);
    printf("  Update TID: %016llx v%u\n", (unsigned long long)res.update_title_id, res.update_version);
    printf("  Build IDs:  rtld=%s main=%s subsdk0=%s sdk=%s\n",
           res.rtld_build_id, res.main_build_id, res.subsdk0_build_id, res.sdk_build_id);
    printf("  RomFS:      %llu files, %llu bytes (%.2f MiB)\n",
           (unsigned long long)res.romfs_files, (unsigned long long)res.romfs_bytes,
           (double)res.romfs_bytes / 1048576.0);
    printf("  AOC count:  %d\n", res.aoc_count);

    int neg_rc = run_negative_suite(keys_fd, base_fd, upd_fd, dlc_fds, dlc_count, patches_dir, dest_root);

    close(keys_fd); close(base_fd); close(upd_fd);
    for (int i = 0; i < dlc_count && i < 32; i++) close(dlc_fds[i]);
    return neg_rc;
}
