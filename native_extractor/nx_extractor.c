#define _FILE_OFFSET_BITS 64
#include "nx_extractor.h"
#include "nx_crypto.h"
#include "nx_patch_stream.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

static const struct {
    const char *name;
    const char *expected_build_id;
} EXPECTED_MODS[4] = {
    { "rtld",    "567cdd4c5251494c5cd70822e3ccc740" },
    { "main",    "306dfcfc9eba4a1a7d0ae7ffcee4b9e4" },
    { "subsdk0", "d0f13015e5badf2243d60a2bc9b978dd" },
    { "sdk",     "3644a6760469451cabc99f00c540acc2" },
};

static int rm_rf(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return errno == ENOENT ? 0 : -1;
    if (!S_ISDIR(st.st_mode)) return unlink(path);
    DIR *d = opendir(path);
    if (!d) return -1;
    struct dirent *e;
    char child[1280];
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        snprintf(child, sizeof child, "%s/%s", path, e->d_name);
        rm_rf(child);
    }
    closedir(d);
    return rmdir(path);
}

static int copy_file(const char *src, const char *dst, char *err, size_t errcap) {
    int in = open(src, O_RDONLY);
    if (in < 0) { snprintf(err, errcap, "cannot open %s", src); return -1; }
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) { close(in); snprintf(err, errcap, "cannot create %s", dst); return -2; }
    uint8_t buf[32768];
    ssize_t r;
    int rc = 0;
    while ((r = read(in, buf, sizeof buf)) > 0) {
        ssize_t put = 0;
        while (put < r) {
            ssize_t w = write(out, buf + put, (size_t)(r - put));
            if (w <= 0) { rc = -3; snprintf(err, errcap, "write failed: %s", dst); break; }
            put += w;
        }
        if (rc != 0) break;
    }
    if (r < 0 && rc == 0) { rc = -4; snprintf(err, errcap, "read failed: %s", src); }
    close(out);
    close(in);
    return rc;
}

static int read_nso_build_id(const char *path, char out_hex[33]) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    uint8_t hdr[0x50];
    ssize_t r = pread(fd, hdr, sizeof hdr, 0);
    close(fd);
    if (r != (ssize_t)sizeof hdr || memcmp(hdr, "NSO0", 4) != 0) return -2;
    static const char H[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) {
        out_hex[i * 2]     = H[hdr[0x40 + i] >> 4];
        out_hex[i * 2 + 1] = H[hdr[0x40 + i] & 0xF];
    }
    out_hex[32] = '\0';
    return 0;
}

uint64_t nx_query_free_space(const char *path) {
    struct statvfs sv;
    if (statvfs(path, &sv) != 0) return 0;
    return (uint64_t)sv.f_bavail * (uint64_t)sv.f_frsize;
}

static void emit_prog(const nx_extract_params *p, const char *stage,
                      uint64_t fd, uint64_t ft, uint64_t bd, uint64_t bt) {
    if (p->progress_cb) p->progress_cb(p->progress_user, stage, fd, ft, bd, bt);
}

static int is_cancelled(const nx_extract_params *p) {
    return p->cancel_flag && *p->cancel_flag;
}

int nx_extract_hybrid_game(const nx_extract_params *p, nx_extract_result *res,
                           char *err, size_t errcap) {
    if (res) memset(res, 0, sizeof *res);
    if (!p || !p->dest_root || !p->patches_dir) {
        snprintf(err, errcap, "invalid extractor parameters");
        return -1;
    }
    if (mkdir(p->dest_root, 0755) != 0 && errno != EEXIST) {
        snprintf(err, errcap, "cannot create destination root: %s", p->dest_root);
        return -2;
    }

    /* 1. Check free storage space BEFORE starting */
    uint64_t need = p->min_free_bytes ? p->min_free_bytes : NX_DEFAULT_MIN_FREE;
    uint64_t avail = nx_query_free_space(p->dest_root);
    if (avail < need) {
        snprintf(err, errcap, "insufficient free storage: %.2f GiB available, %.2f GiB required",
                 avail / 1073741824.0, need / 1073741824.0);
        return -3;
    }

    emit_prog(p, "validate_keys", 0, 1, 0, 1);

    /* 2. Load prod.keys from raw fd */
    nx_keyset ks;
    if (nx_keyset_load_fd(p->keys_fd, &ks, err, errcap) != 0) return -4;

    /* 3. Open & scan Base NSP/XCI (and optional separate Update NSP/XCI) via CNMT BEFORE writing gigabytes */
    emit_prog(p, "validate_nsp", 0, 2, 0, 2);
    nx_nsp base_nsp, upd_nsp;
    memset(&base_nsp, 0, sizeof base_nsp);
    memset(&upd_nsp, 0, sizeof upd_nsp);
    int has_sep_upd = (p->update_fd >= 0);

    if (nx_nsp_open(p->base_fd, &base_nsp, err, errcap) != 0) return -5;
    if (has_sep_upd && nx_nsp_open(p->update_fd, &upd_nsp, err, errcap) != 0) {
        nx_nsp_close(&base_nsp);
        return -6;
    }

    nx_cnmt_info base_cnmts[32];
    int n_base_cnmt = nx_nsp_scan_cnmts(&base_nsp, &ks, base_cnmts, 32, err, errcap);
    if (n_base_cnmt <= 0) {
        nx_nsp_close(&base_nsp); nx_nsp_close(&upd_nsp); return -7;
    }

    nx_cnmt_info base_cnmt, upd_cnmt;
    memset(&base_cnmt, 0, sizeof base_cnmt);
    memset(&upd_cnmt, 0, sizeof upd_cnmt);
    int found_base = 0, found_upd = 0, upd_from_base = 0;
    uint32_t wrong_upd_ver = 0;

    typedef struct {
        int aoc_idx;
        int from_base;
        int ext_dlc_pos;
        char nca_id[33];
    } aoc_src_t;
    aoc_src_t aoc_srcs[32];
    int aoc_n = 0;

    for (int i = 0; i < n_base_cnmt; i++) {
        const nx_cnmt_info *ci = &base_cnmts[i];
        if (ci->meta_type == 0x80 && ci->title_id == NX_EXPECTED_BASE_TID) {
            base_cnmt = *ci;
            found_base = 1;
        } else if (ci->meta_type == 0x81 && ci->title_id == NX_EXPECTED_UPD_TID && ci->app_id == NX_EXPECTED_BASE_TID) {
            if (ci->version == NX_EXPECTED_UPD_VER) {
                if (!has_sep_upd) { upd_cnmt = *ci; found_upd = 1; upd_from_base = 1; }
            } else {
                wrong_upd_ver = ci->version;
            }
        } else if (ci->meta_type == 0x82 && ci->app_id == NX_EXPECTED_BASE_TID && aoc_n < 32) {
            int idx = (int)(ci->title_id & 0xFFFull);
            if (idx > 0 && idx <= 2000) {
                int dup = 0;
                for (int j = 0; j < aoc_n; j++) if (aoc_srcs[j].aoc_idx == idx) dup = 1;
                if (!dup) {
                    aoc_srcs[aoc_n].aoc_idx = idx;
                    aoc_srcs[aoc_n].from_base = 1;
                    aoc_srcs[aoc_n].ext_dlc_pos = -1;
                    memcpy(aoc_srcs[aoc_n].nca_id, ci->content_nca_id, 33);
                    aoc_n++;
                }
            }
        }
    }

    if (!found_base) {
        snprintf(err, errcap, "Base NSP/XCI mismatch: expected Application %016llx, got type=0x%02x tid=%016llx",
                 (unsigned long long)NX_EXPECTED_BASE_TID, base_cnmts[0].meta_type, (unsigned long long)base_cnmts[0].title_id);
        nx_nsp_close(&base_nsp); nx_nsp_close(&upd_nsp); return -8;
    }

    if (has_sep_upd) {
        nx_cnmt_info upd_cnmts[16];
        int n_upd_cnmt = nx_nsp_scan_cnmts(&upd_nsp, &ks, upd_cnmts, 16, err, errcap);
        if (n_upd_cnmt <= 0) {
            nx_nsp_close(&base_nsp); nx_nsp_close(&upd_nsp); return -9;
        }
        for (int i = 0; i < n_upd_cnmt; i++) {
            const nx_cnmt_info *ci = &upd_cnmts[i];
            if (ci->meta_type == 0x81 && ci->title_id == NX_EXPECTED_UPD_TID &&
                ci->app_id == NX_EXPECTED_BASE_TID && ci->version == NX_EXPECTED_UPD_VER) {
                upd_cnmt = *ci;
                found_upd = 1;
                upd_from_base = 0;
                break;
            }
        }
        if (!found_upd) {
            snprintf(err, errcap, "UPDATE_112_MISSING:Update NSP mismatch: expected Patch %016llx v%u (1.1.2), got type=0x%02x tid=%016llx v%u",
                     (unsigned long long)NX_EXPECTED_UPD_TID, NX_EXPECTED_UPD_VER,
                     upd_cnmts[0].meta_type, (unsigned long long)upd_cnmts[0].title_id, upd_cnmts[0].version);
            nx_nsp_close(&base_nsp); nx_nsp_close(&upd_nsp); return -10;
        }
    } else if (!found_upd) {
        if (wrong_upd_ver != 0) {
            snprintf(err, errcap, "UPDATE_112_MISSING:Merged file contains Update v%u, but this port requires Update v1.1.2 (v%u). Please select Update v1.1.2 (.nsp) or a Merged/Super NSP/XCI with v1.1.2.",
                     wrong_upd_ver, NX_EXPECTED_UPD_VER);
        } else {
            snprintf(err, errcap, "UPDATE_112_MISSING:Base Game v1.0.0 (%016llx v%u) detected without Update v1.1.2 (v%u). Please select Update v1.1.2 (.nsp) or a Merged/Super NSP/XCI with v1.1.2; DLC is optional.",
                     (unsigned long long)base_cnmt.title_id, base_cnmt.version, NX_EXPECTED_UPD_VER);
        }
        nx_nsp_close(&base_nsp); nx_nsp_close(&upd_nsp); return -10;
    }

    /* Validate Base & Update Program NCAs before staging (prefer exact CNMT NCA ID so Merged/Super containers never confuse Base vs Update Program NCA) */
    nx_nca base_prog, upd_prog;
    int base_ok = (base_cnmt.content_nca_id[0] != '\0')
        ? (nx_nsp_open_nca_by_id(&base_nsp, &ks, base_cnmt.content_nca_id, &base_prog, err, errcap) == 0)
        : (nx_nsp_open_nca_by_type(&base_nsp, &ks, 0 /* Program */, NX_EXPECTED_BASE_TID, &base_prog, err, errcap) == 0);
    const nx_nsp *upd_container = upd_from_base ? &base_nsp : &upd_nsp;
    int upd_ok = 0;
    if (base_ok) {
        upd_ok = (upd_cnmt.content_nca_id[0] != '\0')
            ? (nx_nsp_open_nca_by_id(upd_container, &ks, upd_cnmt.content_nca_id, &upd_prog, err, errcap) == 0)
            : (nx_nsp_open_nca_by_type(upd_container, &ks, 0 /* Program */, NX_EXPECTED_BASE_TID, &upd_prog, err, errcap) == 0);
    }
    if (!base_ok || !upd_ok) {
        nx_nsp_close(&base_nsp); nx_nsp_close(&upd_nsp); return -11;
    }

    /* 4. Pre-validate all optional external DLC (AOC) NSPs/XCIs (0 DLCs is 100% allowed) */
    if (p->dlc_count > 32) {
        snprintf(err, errcap, "too many DLC files (%d > 32)", p->dlc_count);
        nx_nsp_close(&base_nsp); nx_nsp_close(&upd_nsp); return -12;
    }
    for (int i = 0; i < p->dlc_count; i++) {
        nx_nsp dnsp;
        if (nx_nsp_open(p->dlc_fds[i], &dnsp, err, errcap) != 0) {
            nx_nsp_close(&base_nsp); nx_nsp_close(&upd_nsp); return -13;
        }
        nx_cnmt_info dcnmt;
        if (nx_nsp_read_cnmt(&dnsp, &ks, &dcnmt, err, errcap) != 0) {
            nx_nsp_close(&dnsp); nx_nsp_close(&base_nsp); nx_nsp_close(&upd_nsp); return -14;
        }
        nx_nsp_close(&dnsp);
        if (dcnmt.meta_type != 0x82 || dcnmt.app_id != NX_EXPECTED_BASE_TID) {
            snprintf(err, errcap, "DLC #%d mismatch: expected AOC for %016llx, got type=0x%02x app=%016llx",
                     i + 1, (unsigned long long)NX_EXPECTED_BASE_TID, dcnmt.meta_type, (unsigned long long)dcnmt.app_id);
            nx_nsp_close(&base_nsp); nx_nsp_close(&upd_nsp); return -15;
        }
        int aoc_idx = (int)(dcnmt.title_id & 0xFFFull);
        if (aoc_idx <= 0 || aoc_idx > 2000) {
            snprintf(err, errcap, "DLC #%d invalid AOC index %d", i + 1, aoc_idx);
            nx_nsp_close(&base_nsp); nx_nsp_close(&upd_nsp); return -16;
        }
        int already_embedded = 0;
        for (int j = 0; j < aoc_n; j++) {
            if (aoc_srcs[j].aoc_idx == aoc_idx) {
                if (aoc_srcs[j].from_base) { already_embedded = 1; break; } /* external DLC already inside SuperXCI: ignore duplicate safely */
                snprintf(err, errcap, "duplicate DLC detected (AOC index %d, tid %016llx)",
                         aoc_idx, (unsigned long long)dcnmt.title_id);
                nx_nsp_close(&base_nsp); nx_nsp_close(&upd_nsp); return -17;
            }
        }
        if (!already_embedded && aoc_n < 32) {
            aoc_srcs[aoc_n].aoc_idx = aoc_idx;
            aoc_srcs[aoc_n].from_base = 0;
            aoc_srcs[aoc_n].ext_dlc_pos = i;
            memcpy(aoc_srcs[aoc_n].nca_id, dcnmt.content_nca_id, 33);
            aoc_n++;
        }
    }

    /* 5. Prepare clean atomic staging folder: dest_root/extract_tmp */
    char tmp_dir[900], final_dir[900], old_dir[900];
    snprintf(tmp_dir,   sizeof tmp_dir,   "%s/extract_tmp",  p->dest_root);
    snprintf(final_dir, sizeof final_dir, "%s/game_data",    p->dest_root);
    snprintf(old_dir,   sizeof old_dir,   "%s/game_data_old", p->dest_root);
    rm_rf(tmp_dir);
    if (mkdir(tmp_dir, 0755) != 0) {
        snprintf(err, errcap, "cannot create staging dir %s", tmp_dir);
        nx_nsp_close(&base_nsp); nx_nsp_close(&upd_nsp); return -18;
    }

    int rc = 0;
    nx_bktr bktr;
    memset(&bktr, 0, sizeof bktr);
    int bktr_opened = 0;
    uint64_t romfs_files = 0, romfs_bytes = 0;
    char bids[4][33];
    memset(bids, 0, sizeof bids);

    /* 6. Extract ExeFS (Update Program NCA section 0 has the full patched ExeFS) & verify Build IDs */
    emit_prog(p, "exefs", 0, 5, 0, 5);
    char exefs_dir[1024], mods_dir[1024], romfs_dir[1024], aoc_dir[1024];
    snprintf(exefs_dir, sizeof exefs_dir, "%s/ExeFS", tmp_dir);
    snprintf(mods_dir,  sizeof mods_dir,  "%s/mods",  tmp_dir);
    snprintf(romfs_dir, sizeof romfs_dir, "%s/RomFS", tmp_dir);
    snprintf(aoc_dir,   sizeof aoc_dir,   "%s/aoc",   tmp_dir);

    if (nx_nca_extract_pfs0(&upd_prog, 0, exefs_dir, p->cancel_flag, err, errcap) != 0) {
        rc = -19; goto cleanup;
    }

    for (int i = 0; i < 4; i++) {
        char mpath[1152];
        snprintf(mpath, sizeof mpath, "%s/%s", exefs_dir, EXPECTED_MODS[i].name);
        if (read_nso_build_id(mpath, bids[i]) != 0) {
            snprintf(err, errcap, "missing or invalid NSO module: %s", EXPECTED_MODS[i].name);
            rc = -20; goto cleanup;
        }
        if (strcmp(bids[i], EXPECTED_MODS[i].expected_build_id) != 0) {
            snprintf(err, errcap, "Build ID mismatch for %s: expected %s, got %s (wrong Update version?)",
                     EXPECTED_MODS[i].name, EXPECTED_MODS[i].expected_build_id, bids[i]);
            rc = -21; goto cleanup;
        }
    }

    /* 7. Apply deterministic *.nxpatch recipes into extract_tmp/mods/ using streaming <2 MiB RAM applier */
    if (mkdir(mods_dir, 0755) != 0 && errno != EEXIST) {
        snprintf(err, errcap, "cannot create mods dir"); rc = -22; goto cleanup;
    }
    for (int i = 0; i < 4; i++) {
        if (is_cancelled(p)) { snprintf(err, errcap, "cancelled"); rc = -99; goto cleanup; }
        emit_prog(p, "patch_nso", (uint64_t)i, 4, (uint64_t)i, 4);
        const char *m = EXPECTED_MODS[i].name;
        char raw_p[1152], pat_p[1152], out_p[1152];
        snprintf(raw_p, sizeof raw_p, "%s/%s", exefs_dir, m);
        snprintf(pat_p, sizeof pat_p, "%s/%s.nxpatch", p->patches_dir, m);
        snprintf(out_p, sizeof out_p, "%s/%s.patched.bin", mods_dir, m);
        if (nx_patch_apply_streaming(raw_p, pat_p, out_p, err, errcap) != 0) {
            rc = -23; goto cleanup;
        }
        /* Copy companion .fixups.tsv and .gates.tsv */
        char src_f[1152], dst_f[1152];
        snprintf(src_f, sizeof src_f, "%s/%s.fixups.tsv", p->patches_dir, m);
        snprintf(dst_f, sizeof dst_f, "%s/%s.fixups.tsv", mods_dir, m);
        if (copy_file(src_f, dst_f, err, errcap) != 0) { rc = -24; goto cleanup; }
        snprintf(src_f, sizeof src_f, "%s/%s.gates.tsv", p->patches_dir, m);
        snprintf(dst_f, sizeof dst_f, "%s/%s.gates.tsv", mods_dir, m);
        if (copy_file(src_f, dst_f, err, errcap) != 0) { rc = -25; goto cleanup; }
    }
    emit_prog(p, "patch_nso", 4, 4, 4, 4);

    /* 8. Open BKTR overlay (Base + Update RomFS) and stream all 13,720 RomFS files */
    if (nx_bktr_open(&base_prog, 1, &upd_prog, 1, &bktr, err, errcap) != 0) {
        rc = -26; goto cleanup;
    }
    bktr_opened = 1;
    if (nx_bktr_extract_romfs(&bktr, romfs_dir, p->progress_cb, p->progress_user,
                              p->cancel_flag, &romfs_files, &romfs_bytes, err, errcap) != 0) {
        rc = is_cancelled(p) ? -99 : -27; goto cleanup;
    }

    /* 9. Extract all validated DLCs (embedded in Merged/Super container or separate files) into extract_tmp/aoc/<index>/ */
    if (mkdir(aoc_dir, 0755) != 0 && errno != EEXIST) {
        snprintf(err, errcap, "cannot create aoc dir"); rc = -28; goto cleanup;
    }
    for (int i = 0; i < aoc_n; i++) {
        if (is_cancelled(p)) { snprintf(err, errcap, "cancelled"); rc = -99; goto cleanup; }
        emit_prog(p, "aoc", (uint64_t)i, (uint64_t)aoc_n, (uint64_t)i, (uint64_t)aoc_n);
        nx_nsp dnsp;
        memset(&dnsp, 0, sizeof dnsp);
        const nx_nsp *src_nsp = NULL;
        if (aoc_srcs[i].from_base) {
            src_nsp = &base_nsp;
        } else {
            if (nx_nsp_open(p->dlc_fds[aoc_srcs[i].ext_dlc_pos], &dnsp, err, errcap) != 0) { rc = -29; goto cleanup; }
            src_nsp = &dnsp;
        }
        nx_nca dnca;
        int dnca_ok = 0;
        if (aoc_srcs[i].nca_id[0] != '\0') {
            dnca_ok = (nx_nsp_open_nca_by_id(src_nsp, &ks, aoc_srcs[i].nca_id, &dnca, err, errcap) == 0);
        }
        if (!dnca_ok) {
            /* Fallback: AOC data NCA is PublicData (5) or Data (4) */
            dnca_ok = (nx_nsp_open_nca_by_type(src_nsp, &ks, 5, 0, &dnca, err, errcap) == 0 ||
                       nx_nsp_open_nca_by_type(src_nsp, &ks, 4, 0, &dnca, err, errcap) == 0);
        }
        if (!dnca_ok) {
            if (!aoc_srcs[i].from_base) nx_nsp_close(&dnsp);
            rc = -30; goto cleanup;
        }
        char sub[1152];
        snprintf(sub, sizeof sub, "%s/%d", aoc_dir, aoc_srcs[i].aoc_idx);
        uint64_t df = 0, db = 0;
        int drc = nx_nca_extract_romfs(&dnca, 0, sub, NULL, NULL, p->cancel_flag, &df, &db, err, errcap);
        if (!aoc_srcs[i].from_base) nx_nsp_close(&dnsp);
        if (drc != 0) { rc = is_cancelled(p) ? -99 : -31; goto cleanup; }
    }
    emit_prog(p, "aoc", (uint64_t)aoc_n, (uint64_t)aoc_n, (uint64_t)aoc_n, (uint64_t)aoc_n);

    /* 10. Write atomic completion manifest .ready.json inside extract_tmp and fsync */
    emit_prog(p, "commit", 0, 1, 0, 1);
    {
        char ready_p[1024];
        snprintf(ready_p, sizeof ready_p, "%s/.ready.json", tmp_dir);
        int fd = open(ready_p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) { snprintf(err, errcap, "cannot write .ready.json"); rc = -32; goto cleanup; }
        char jbuf[1536];
        char aoc_list[256] = "";
        for (int i = 0; i < aoc_n; i++) {
            char tmp_num[16];
            snprintf(tmp_num, sizeof tmp_num, "%s%d", i ? "," : "", aoc_srcs[i].aoc_idx);
            strncat(aoc_list, tmp_num, sizeof(aoc_list) - strlen(aoc_list) - 1);
        }
        int jn = snprintf(jbuf, sizeof jbuf,
            "{\n"
            "  \"title_id\": \"%016llx\",\n"
            "  \"update_title_id\": \"%016llx\",\n"
            "  \"update_version\": %u,\n"
            "  \"display_version\": \"1.1.2\",\n"
            "  \"build_ids\": {\n"
            "    \"rtld\": \"%s\",\n"
            "    \"main\": \"%s\",\n"
            "    \"subsdk0\": \"%s\",\n"
            "    \"sdk\": \"%s\"\n"
            "  },\n"
            "  \"romfs_files\": %llu,\n"
            "  \"romfs_bytes\": %llu,\n"
            "  \"aoc_indices\": [%s],\n"
            "  \"timestamp\": %lld\n"
            "}\n",
            (unsigned long long)NX_EXPECTED_BASE_TID,
            (unsigned long long)NX_EXPECTED_UPD_TID,
            NX_EXPECTED_UPD_VER,
            bids[0], bids[1], bids[2], bids[3],
            (unsigned long long)romfs_files,
            (unsigned long long)romfs_bytes,
            aoc_list,
            (long long)time(NULL));
        if (jn <= 0 || write(fd, jbuf, (size_t)jn) != jn || fsync(fd) != 0) {
            close(fd); snprintf(err, errcap, "failed flushing .ready.json"); rc = -33; goto cleanup;
        }
        close(fd);
    }

    /* 11. Atomic swap commit: game_data -> game_data_old, extract_tmp -> game_data, rm game_data_old */
    rm_rf(old_dir);
    int had_old = (access(final_dir, F_OK) == 0);
    if (had_old && rename(final_dir, old_dir) != 0) {
        snprintf(err, errcap, "failed moving previous game_data aside (errno=%d)", errno);
        rc = -34; goto cleanup;
    }
    if (rename(tmp_dir, final_dir) != 0) {
        int e = errno;
        if (had_old) rename(old_dir, final_dir); /* rollback previous valid install! */
        snprintf(err, errcap, "atomic commit rename failed (errno=%d)", e);
        rc = -35; goto cleanup;
    }
    if (had_old) rm_rf(old_dir);
    emit_prog(p, "done", 1, 1, 1, 1);

    if (res) {
        res->base_title_id = base_cnmt.title_id;
        res->update_title_id = upd_cnmt.title_id;
        res->update_version = upd_cnmt.version;
        res->romfs_files = romfs_files;
        res->romfs_bytes = romfs_bytes;
        res->aoc_count = aoc_n;
        for (int i = 0; i < aoc_n; i++) res->aoc_indices[i] = aoc_srcs[i].aoc_idx;
        memcpy(res->rtld_build_id,    bids[0], 33);
        memcpy(res->main_build_id,    bids[1], 33);
        memcpy(res->subsdk0_build_id, bids[2], 33);
        memcpy(res->sdk_build_id,     bids[3], 33);
    }

cleanup:
    if (bktr_opened) nx_bktr_close(&bktr);
    nx_nsp_close(&base_nsp);
    nx_nsp_close(&upd_nsp);
    if (rc != 0) rm_rf(tmp_dir); /* guarantee extract_tmp is cleaned on any failure/cancel */
    return rc;
}
