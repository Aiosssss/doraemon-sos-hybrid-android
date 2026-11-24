#define _FILE_OFFSET_BITS 64
#include "nx_nsp.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
static inline uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline uint64_t rd64(const uint8_t *p) {
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

static int pread_all(int fd, void *buf, size_t len, uint64_t off) {
    uint8_t *p = (uint8_t *)buf;
    size_t got = 0;
    while (got < len) {
        ssize_t r = pread(fd, p + got, len - got, (off_t)(off + got));
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    return 0;
}

static int write_all(int fd, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    size_t put = 0;
    while (put < len) {
        ssize_t w = write(fd, p + put, len - put);
        if (w <= 0) return -1;
        put += (size_t)w;
    }
    return 0;
}

static int mkdir_p(const char *path) {
    char tmp[1024];
    size_t n = strlen(path);
    if (n == 0 || n >= sizeof(tmp)) return -1;
    memcpy(tmp, path, n + 1);
    for (size_t i = 1; i < n; i++) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
            tmp[i] = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

/* ---------------- NSP (PFS0) & XCI (HEAD -> HFS0/secure) ---------------- */
static int open_hfs0_at(int fd, uint64_t base_off, nx_nsp *nsp, char *err, size_t errcap) {
    uint8_t hdr[16];
    if (pread_all(fd, hdr, 16, base_off) != 0 || memcmp(hdr, "HFS0", 4) != 0) {
        snprintf(err, errcap, "invalid HFS0 partition header at 0x%llx", (unsigned long long)base_off);
        return -2;
    }
    uint32_t nfiles = rd32(hdr + 4);
    uint32_t strtab_sz = rd32(hdr + 8);
    if (nfiles == 0 || nfiles > 4096 || strtab_sz > (4u << 20)) {
        snprintf(err, errcap, "unreasonable HFS0 table (files=%u, strtab=%u)", nfiles, strtab_sz);
        return -3;
    }
    size_t tab_sz = (size_t)nfiles * 64; /* HFS0 entry size is 0x40 bytes */
    uint8_t *raw = malloc(tab_sz + strtab_sz);
    if (!raw) { snprintf(err, errcap, "OOM reading HFS0 table"); return -4; }
    if (pread_all(fd, raw, tab_sz + strtab_sz, base_off + 16) != 0) {
        free(raw); snprintf(err, errcap, "truncated HFS0 header table"); return -5;
    }
    nx_pfs0_entry *ents = calloc(nfiles, sizeof *ents);
    if (!ents) { free(raw); snprintf(err, errcap, "OOM HFS0 entries"); return -6; }

    uint64_t data_base = base_off + 16ull + tab_sz + strtab_sz;
    const char *st = (const char *)(raw + tab_sz);
    for (uint32_t i = 0; i < nfiles; i++) {
        const uint8_t *e = raw + i * 64;
        uint64_t off = rd64(e);
        uint64_t sz = rd64(e + 8);
        uint32_t noff = rd32(e + 16);
        if (noff >= strtab_sz) { free(ents); free(raw); snprintf(err, errcap, "HFS0 strtab OOB"); return -7; }
        size_t maxn = strtab_sz - noff;
        size_t nl = strnlen(st + noff, maxn);
        if (nl >= sizeof(ents[i].name)) nl = sizeof(ents[i].name) - 1;
        memcpy(ents[i].name, st + noff, nl);
        ents[i].name[nl] = '\0';
        ents[i].offset = data_base + off;
        ents[i].size = sz;
    }
    free(raw);
    nsp->fd = fd;
    nsp->count = nfiles;
    nsp->entries = ents;
    return 0;
}

int nx_nsp_open(int fd, nx_nsp *nsp, char *err, size_t errcap) {
    memset(nsp, 0, sizeof *nsp);
    if (fd < 0) { snprintf(err, errcap, "invalid NSP/XCI file descriptor"); return -1; }
    uint8_t hdr[16];
    if (pread_all(fd, hdr, 16, 0) != 0) {
        snprintf(err, errcap, "cannot read NSP/XCI header");
        return -2;
    }
    /* Check for XCI cartridge image ("HEAD" at +0x100 -> Root HFS0 -> "secure" HFS0) */
    if (memcmp(hdr, "PFS0", 4) != 0) {
        uint8_t xci_hdr[0x40];
        if (pread_all(fd, xci_hdr, sizeof xci_hdr, 0x100) == 0 && memcmp(xci_hdr, "HEAD", 4) == 0) {
            uint64_t root_hfs_off = rd64(xci_hdr + 0x30);
            nx_nsp root_hfs;
            memset(&root_hfs, 0, sizeof root_hfs);
            if (open_hfs0_at(fd, root_hfs_off, &root_hfs, err, errcap) != 0) return -2;
            uint64_t secure_off = 0;
            int found_secure = 0;
            for (uint32_t i = 0; i < root_hfs.count; i++) {
                if (strcmp(root_hfs.entries[i].name, "secure") == 0) {
                    secure_off = root_hfs.entries[i].offset;
                    found_secure = 1;
                    break;
                }
            }
            nx_nsp_close(&root_hfs);
            if (!found_secure) {
                snprintf(err, errcap, "XCI image missing 'secure' HFS0 partition");
                return -2;
            }
            return open_hfs0_at(fd, secure_off, nsp, err, errcap);
        }
        if (memcmp(hdr, "HFS0", 4) == 0) {
            nx_nsp root_hfs;
            memset(&root_hfs, 0, sizeof root_hfs);
            if (open_hfs0_at(fd, 0, &root_hfs, err, errcap) != 0) return -2;
            for (uint32_t i = 0; i < root_hfs.count; i++) {
                if (strcmp(root_hfs.entries[i].name, "secure") == 0) {
                    uint64_t secure_off = root_hfs.entries[i].offset;
                    nx_nsp_close(&root_hfs);
                    return open_hfs0_at(fd, secure_off, nsp, err, errcap);
                }
            }
            *nsp = root_hfs;
            return 0;
        }
        snprintf(err, errcap, "invalid NSP/XCI header (expected PFS0 or XCI HEAD)");
        return -2;
    }
    uint32_t nfiles = rd32(hdr + 4);
    uint32_t strtab_sz = rd32(hdr + 8);
    if (nfiles == 0 || nfiles > 4096 || strtab_sz > (4u << 20)) {
        snprintf(err, errcap, "unreasonable PFS0 table (files=%u, strtab=%u)", nfiles, strtab_sz);
        return -3;
    }
    size_t tab_sz = (size_t)nfiles * 24;
    uint8_t *raw = malloc(tab_sz + strtab_sz);
    if (!raw) { snprintf(err, errcap, "OOM reading NSP table"); return -4; }
    if (pread_all(fd, raw, tab_sz + strtab_sz, 16) != 0) {
        free(raw); snprintf(err, errcap, "truncated NSP header table"); return -5;
    }
    nx_pfs0_entry *ents = calloc(nfiles, sizeof *ents);
    if (!ents) { free(raw); snprintf(err, errcap, "OOM NSP entries"); return -6; }

    uint64_t data_base = 16ull + tab_sz + strtab_sz;
    const char *st = (const char *)(raw + tab_sz);
    for (uint32_t i = 0; i < nfiles; i++) {
        const uint8_t *e = raw + i * 24;
        uint64_t off = rd64(e);
        uint64_t sz = rd64(e + 8);
        uint32_t noff = rd32(e + 16);
        if (noff >= strtab_sz) { free(ents); free(raw); snprintf(err, errcap, "NSP strtab OOB"); return -7; }
        size_t maxn = strtab_sz - noff;
        size_t nl = strnlen(st + noff, maxn);
        if (nl >= sizeof(ents[i].name)) nl = sizeof(ents[i].name) - 1;
        memcpy(ents[i].name, st + noff, nl);
        ents[i].name[nl] = '\0';
        ents[i].offset = data_base + off;
        ents[i].size = sz;
    }
    free(raw);
    nsp->fd = fd;
    nsp->count = nfiles;
    nsp->entries = ents;
    return 0;
}

void nx_nsp_close(nx_nsp *nsp) {
    if (nsp) { free(nsp->entries); memset(nsp, 0, sizeof *nsp); }
}

/* ---------------- Ticket (.tik) Title Key Resolution ---------------- */
static int find_titlekey_in_nsp(const nx_nsp *nsp, const uint8_t rights_id[16],
                                const nx_keyset *ks, uint8_t out_key[16],
                                char *err, size_t errcap) {
    for (uint32_t i = 0; i < nsp->count; i++) {
        const char *nm = nsp->entries[i].name;
        size_t nl = strlen(nm);
        if (nl < 4 || strcmp(nm + nl - 4, ".tik") != 0) continue;
        if (nsp->entries[i].size < 0x200 || nsp->entries[i].size > 65536) continue;
        uint8_t *tik = malloc((size_t)nsp->entries[i].size);
        if (!tik) return -1;
        if (pread_all(nsp->fd, tik, (size_t)nsp->entries[i].size, nsp->entries[i].offset) != 0) {
            free(tik); continue;
        }
        uint32_t sig_type = rd32(tik);
        size_t hdr_off = 0;
        switch (sig_type) {
            case 0x010000: case 0x010003: hdr_off = 0x240; break; /* RSA-4096 */
            case 0x010001: case 0x010004: hdr_off = 0x140; break; /* RSA-2048 */
            case 0x010002: case 0x010005: hdr_off = 0x080; break; /* ECDSA */
            default: free(tik); continue;
        }
        if (hdr_off + 0x180 > nsp->entries[i].size) { free(tik); continue; }
        const uint8_t *body = tik + hdr_off;
        /* RightsId is at body + 0x160; enc titlekey block at body + 0x40; keygen at body + 0x145 (or 0x146) */
        if (memcmp(body + 0x160, rights_id, 16) != 0) { free(tik); continue; }
        uint8_t tk_type = body[0x141];
        if (tk_type != 0) {
            free(tik);
            snprintf(err, errcap, "personalized ticket (type %u) not supported", tk_type);
            return -2;
        }
        uint8_t kg = body[0x145];
        if (kg == 0) kg = body[0x146];
        uint8_t idx = kg > 0 ? (uint8_t)(kg - 1) : 0;
        if (idx >= NX_MAX_KEYGEN || !ks->has_titlekek[idx]) {
            free(tik);
            snprintf(err, errcap, "prod.keys missing titlekek_%02x for ticket", idx);
            return -3;
        }
        nx_aes128_ctx kek;
        nx_aes128_init(&kek, ks->titlekek[idx]);
        nx_aes128_ecb_decrypt(&kek, body + 0x40, out_key);
        free(tik);
        return 0;
    }
    snprintf(err, errcap, "no matching common ticket (.tik) found in NSP for RightsId");
    return -4;
}

/* ---------------- NCA3 Open & Section I/O ---------------- */
static void make_nca_ctr_nonce(const uint8_t section_ctr[8], uint8_t nonce_be[8]) {
    /* Nintendo Switch NCA3 CTR upper 8 bytes: byte-reversed 64-bit section_ctr */
    for (int i = 0; i < 8; i++) nonce_be[i] = section_ctr[7 - i];
}

static void make_bktr_ctr_nonce(const uint8_t section_ctr[8], uint32_t ctr_val, uint8_t nonce_be[8]) {
    /* Aes128CtrEx: ctr[0..3] = section_ctr[7..4] (generation BE), ctr[4..7] = ctr_val BE */
    nonce_be[0] = section_ctr[7];
    nonce_be[1] = section_ctr[6];
    nonce_be[2] = section_ctr[5];
    nonce_be[3] = section_ctr[4];
    nonce_be[4] = (uint8_t)(ctr_val >> 24);
    nonce_be[5] = (uint8_t)(ctr_val >> 16);
    nonce_be[6] = (uint8_t)(ctr_val >> 8);
    nonce_be[7] = (uint8_t)(ctr_val);
}

static int nca_read_section_plain_ctr(const nx_nca *nca, int sec_idx, uint64_t rel_off,
                                      uint8_t *dst, size_t len) {
    const nx_nca_section *s = &nca->sec[sec_idx];
    uint64_t nca_off = s->media_start + rel_off;
    if (nca_off + len > s->media_end) return -1;
    if (pread_all(nca->fd, dst, len, nca->nca_offset + nca_off) != 0) return -2;
    if (s->enc_type == 3 || s->enc_type == 4) {
        uint8_t nonce[8];
        make_nca_ctr_nonce(s->section_ctr, nonce);
        nx_aes128_ctr_xor(&nca->body_aes, nonce, nca_off, dst, len);
    }
    return 0;
}

int nx_nca_open(const nx_nsp *nsp, const nx_pfs0_entry *ent, const nx_keyset *ks,
                nx_nca *nca, char *err, size_t errcap) {
    memset(nca, 0, sizeof *nca);
    if (ent->size < 0xC00) { snprintf(err, errcap, "NCA too small: %s", ent->name); return -1; }
    uint8_t enc_hdr[0xC00], hdr[0xC00];
    if (pread_all(nsp->fd, enc_hdr, 0xC00, ent->offset) != 0) {
        snprintf(err, errcap, "failed to read NCA header: %s", ent->name);
        return -2;
    }
    for (uint64_t sec = 0; sec < 6; sec++) {
        nx_aes128_xts_decrypt_sector(ks->header_key, sec, enc_hdr + sec * 0x200, hdr + sec * 0x200, 0x200);
    }
    if (memcmp(hdr + 0x200, "NCA3", 4) != 0) {
        snprintf(err, errcap, "invalid NCA3 magic in %s (wrong prod.keys header_key or corrupt NSP)", ent->name);
        return -3;
    }
    nca->fd = nsp->fd;
    nca->nca_offset = ent->offset;
    nca->nca_size = ent->size;
    nca->content_type = hdr[0x205];
    uint8_t old_kg = hdr[0x206];
    uint8_t kaek_idx = hdr[0x207];
    nca->title_id = rd64(hdr + 0x210);
    uint8_t new_kg = hdr[0x220];
    uint8_t kg = new_kg > old_kg ? new_kg : old_kg;
    nca->keygen_idx = kg > 0 ? (uint8_t)(kg - 1) : 0;
    memcpy(nca->rights_id, hdr + 0x230, 16);
    nca->has_rights_id = 0;
    for (int i = 0; i < 16; i++) if (nca->rights_id[i] != 0) { nca->has_rights_id = 1; break; }

    if (nca->has_rights_id) {
        if (find_titlekey_in_nsp(nsp, nca->rights_id, ks, nca->body_key, err, errcap) != 0) return -4;
    } else {
        if (nca->keygen_idx >= NX_MAX_KEYGEN) { snprintf(err, errcap, "unsupported keygen %u", nca->keygen_idx); return -5; }
        const uint8_t *kak = NULL;
        if (kaek_idx == 0 && ks->has_kak_app[nca->keygen_idx]) kak = ks->kak_app[nca->keygen_idx];
        else if (kaek_idx == 1 && ks->has_kak_ocean[nca->keygen_idx]) kak = ks->kak_ocean[nca->keygen_idx];
        else if (kaek_idx == 2 && ks->has_kak_sys[nca->keygen_idx]) kak = ks->kak_sys[nca->keygen_idx];
        if (!kak) {
            snprintf(err, errcap, "prod.keys missing key_area_key (type=%u, gen=%02x)", kaek_idx, nca->keygen_idx);
            return -6;
        }
        nx_aes128_ctx kctx;
        nx_aes128_init(&kctx, kak);
        /* Slot 2 at hdr + 0x300 + 2*16 = 0x320 is the AES-CTR key */
        nx_aes128_ecb_decrypt(&kctx, hdr + 0x320, nca->body_key);
    }
    nx_aes128_init(&nca->body_aes, nca->body_key);

    for (int i = 0; i < 4; i++) {
        const uint8_t *fe = hdr + 0x240 + i * 16;
        uint32_t mstart = rd32(fe);
        uint32_t mend = rd32(fe + 4);
        if (mend <= mstart) continue;
        nx_nca_section *s = &nca->sec[i];
        s->exists = 1;
        s->media_start = (uint64_t)mstart * 0x200ull;
        s->media_end = (uint64_t)mend * 0x200ull;
        const uint8_t *fh = hdr + 0x400 + i * 0x200;
        s->fs_type = fh[0x02];
        s->hash_type = fh[0x03];
        s->enc_type = fh[0x04];
        memcpy(s->section_ctr, fh + 0x140, 8);

        if (s->hash_type == 2) {
            /* HierarchicalSha256 (PFS0): master_hash(0x08..0x27), block_sz(0x28), layer_cnt(0x2c),
             * layer0(0x30..0x3f), layer1 PFS0 data(0x40: offset, 0x48: size) */
            s->data_offset = rd64(fh + 0x40);
            s->data_size = rd64(fh + 0x48);
        } else if (s->hash_type == 3) {
            /* IVFC (RomFS): level 5 (data level) is at fh + 0x18 + 5*24 = fh + 0x90 */
            s->data_offset = rd64(fh + 0x90);
            s->data_size = rd64(fh + 0x98);
        }
        if (s->enc_type == 4) {
            /* BKTR PatchInfo at fh + 0x100 */
            s->reloc_off = rd64(fh + 0x100);
            s->reloc_sz  = rd64(fh + 0x108);
            s->enc_off   = rd64(fh + 0x120);
            s->enc_sz    = rd64(fh + 0x128);
        }
    }
    return 0;
}

int nx_nsp_open_nca_by_type(const nx_nsp *nsp, const nx_keyset *ks, uint8_t content_type,
                            uint64_t match_tid, nx_nca *nca, char *err, size_t errcap) {
    char last_err[256] = "no matching NCA found";
    for (uint32_t i = 0; i < nsp->count; i++) {
        const char *nm = nsp->entries[i].name;
        size_t nl = strlen(nm);
        if (nl < 4 || strcmp(nm + nl - 4, ".nca") != 0) continue;
        nx_nca tmp;
        char e[256] = "";
        if (nx_nca_open(nsp, &nsp->entries[i], ks, &tmp, e, sizeof e) != 0) {
            snprintf(last_err, sizeof last_err, "%s", e);
            continue;
        }
        if (tmp.content_type == content_type && (match_tid == 0 || tmp.title_id == match_tid)) {
            *nca = tmp;
            return 0;
        }
    }
    snprintf(err, errcap, "NCA type=%u not found (%s)", content_type, last_err);
    return -1;
}

static int hex_prefix_eq_ci(const char *name, const char hex32[33]) {
    for (int i = 0; i < 32; i++) {
        char a = name[i], b = hex32[i];
        if (!a || !b) return 0;
        if (a >= 'A' && a <= 'F') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'F') b = (char)(b - 'A' + 'a');
        if (a != b) return 0;
    }
    return 1;
}

int nx_nsp_open_nca_by_id(const nx_nsp *nsp, const nx_keyset *ks, const char nca_id_hex[33],
                          nx_nca *nca, char *err, size_t errcap) {
    if (!nca_id_hex || !nca_id_hex[0]) { snprintf(err, errcap, "empty NCA ID"); return -1; }
    for (uint32_t i = 0; i < nsp->count; i++) {
        const char *nm = nsp->entries[i].name;
        size_t nl = strlen(nm);
        if (nl < 36 || strcmp(nm + nl - 4, ".nca") != 0) continue;
        if (hex_prefix_eq_ci(nm, nca_id_hex)) {
            return nx_nca_open(nsp, &nsp->entries[i], ks, nca, err, errcap);
        }
    }
    snprintf(err, errcap, "NCA %s.nca not found in container", nca_id_hex);
    return -2;
}

/* ---------------- CNMT Reader ---------------- */
static int parse_cnmt_from_meta_nca(const nx_nca *meta, nx_cnmt_info *info, char *err, size_t errcap) {
    memset(info, 0, sizeof *info);
    if (!meta->sec[0].exists || meta->sec[0].fs_type != 1) {
        snprintf(err, errcap, "Meta NCA missing PFS0 section 0");
        return -2;
    }
    uint64_t pfs_off = meta->sec[0].data_offset;
    uint64_t pfs_sz = meta->sec[0].data_size;
    if (pfs_sz < 16 || pfs_sz > (2u << 20)) {
        snprintf(err, errcap, "unexpected Meta PFS0 size %llu", (unsigned long long)pfs_sz);
        return -3;
    }
    uint8_t *buf = malloc((size_t)pfs_sz);
    if (!buf) { snprintf(err, errcap, "OOM Meta PFS0"); return -4; }
    if (nca_read_section_plain_ctr(meta, 0, pfs_off, buf, (size_t)pfs_sz) != 0 || memcmp(buf, "PFS0", 4) != 0) {
        free(buf); snprintf(err, errcap, "failed to decrypt Meta PFS0"); return -5;
    }
    uint32_t nf = rd32(buf + 4), stsz = rd32(buf + 8);
    size_t dbase = 16 + (size_t)nf * 24 + stsz;
    const char *st = (const char *)(buf + 16 + (size_t)nf * 24);
    int found = 0;
    for (uint32_t i = 0; i < nf; i++) {
        uint64_t off = rd64(buf + 16 + i * 24);
        uint64_t sz = rd64(buf + 16 + i * 24 + 8);
        uint32_t noff = rd32(buf + 16 + i * 24 + 16);
        if (noff >= stsz || dbase + off + sz > pfs_sz) continue;
        const char *nm = st + noff;
        size_t nl = strnlen(nm, stsz - noff);
        if (nl >= 5 && memcmp(nm + nl - 5, ".cnmt", 5) == 0 && sz >= 0x20) {
            const uint8_t *c = buf + dbase + off;
            info->title_id = rd64(c + 0x00);
            info->version = rd32(c + 0x08);
            info->meta_type = c[0x0C];
            uint16_t ext_hdr_sz = rd16(c + 0x0E);
            uint16_t content_cnt = rd16(c + 0x10);
            info->app_id = info->title_id;
            if ((info->meta_type == 0x81 || info->meta_type == 0x82) && ext_hdr_sz >= 8 && sz >= 0x28) {
                /* PatchMetaExtendedHeader & AddOnContentMetaExtendedHeader: ApplicationId is at +0x20 */
                info->app_id = rd64(c + 0x20);
            }
            /* Extract primary Program (type 1 for 0x80/0x81) or Data (type 2 for 0x82) NCA ID from content records */
            uint8_t want_ct = (info->meta_type == 0x82) ? 2 : 1;
            size_t rec_base = 0x20u + (size_t)ext_hdr_sz;
            static const char H[] = "0123456789abcdef";
            for (uint16_t r = 0; r < content_cnt; r++) {
                size_t roff = rec_base + (size_t)r * 0x38u;
                if (roff + 0x38u > sz) break;
                uint8_t ct = c[roff + 0x36];
                if (ct == want_ct) {
                    for (int k = 0; k < 16; k++) {
                        uint8_t b = c[roff + 0x20 + k];
                        info->content_nca_id[k * 2]     = H[b >> 4];
                        info->content_nca_id[k * 2 + 1] = H[b & 0xF];
                    }
                    info->content_nca_id[32] = '\0';
                    break;
                }
            }
            found = 1;
            break;
        }
    }
    free(buf);
    if (!found) { snprintf(err, errcap, "no .cnmt file found in Meta NCA"); return -6; }
    return 0;
}

int nx_nsp_read_cnmt(const nx_nsp *nsp, const nx_keyset *ks, nx_cnmt_info *info, char *err, size_t errcap) {
    memset(info, 0, sizeof *info);
    nx_nca meta;
    if (nx_nsp_open_nca_by_type(nsp, ks, 1 /* Meta */, 0, &meta, err, errcap) != 0) return -1;
    return parse_cnmt_from_meta_nca(&meta, info, err, errcap);
}

int nx_nsp_scan_cnmts(const nx_nsp *nsp, const nx_keyset *ks, nx_cnmt_info *out, int max_out, char *err, size_t errcap) {
    int count = 0;
    char last_err[256] = "no Meta NCA (.cnmt.nca) found in container";
    for (uint32_t i = 0; i < nsp->count && count < max_out; i++) {
        const char *nm = nsp->entries[i].name;
        size_t nl = strlen(nm);
        if (nl < 4 || strcmp(nm + nl - 4, ".nca") != 0) continue;
        nx_nca tmp;
        char e[256] = "";
        if (nx_nca_open(nsp, &nsp->entries[i], ks, &tmp, e, sizeof e) != 0) {
            snprintf(last_err, sizeof last_err, "%s", e);
            continue;
        }
        if (tmp.content_type != 1 /* Meta */) continue;
        nx_cnmt_info info;
        if (parse_cnmt_from_meta_nca(&tmp, &info, e, sizeof e) == 0) {
            out[count++] = info;
        } else {
            snprintf(last_err, sizeof last_err, "%s", e);
        }
    }
    if (count == 0) {
        snprintf(err, errcap, "%s", last_err);
        return -1;
    }
    return count;
}

/* ---------------- PFS0 Section Extraction (ExeFS) ---------------- */
int nx_nca_extract_pfs0(const nx_nca *nca, int sec_idx, const char *out_dir,
                        volatile int *cancel_flag, char *err, size_t errcap) {
    if (!nca->sec[sec_idx].exists || nca->sec[sec_idx].fs_type != 1) {
        snprintf(err, errcap, "NCA section %d is not a valid PFS0", sec_idx);
        return -1;
    }
    if (mkdir_p(out_dir) != 0) { snprintf(err, errcap, "cannot create dir: %s", out_dir); return -2; }

    uint64_t pfs_base = nca->sec[sec_idx].data_offset;
    uint8_t hdr[16];
    if (nca_read_section_plain_ctr(nca, sec_idx, pfs_base, hdr, 16) != 0 || memcmp(hdr, "PFS0", 4) != 0) {
        snprintf(err, errcap, "invalid PFS0 header in NCA section %d", sec_idx);
        return -3;
    }
    uint32_t nf = rd32(hdr + 4), stsz = rd32(hdr + 8);
    if (nf > 4096 || stsz > (2u << 20)) { snprintf(err, errcap, "PFS0 header out of bounds"); return -4; }
    size_t meta_sz = (size_t)nf * 24 + stsz;
    uint8_t *meta = malloc(meta_sz);
    if (!meta) { snprintf(err, errcap, "OOM PFS0 metadata"); return -5; }
    if (nca_read_section_plain_ctr(nca, sec_idx, pfs_base + 16, meta, meta_sz) != 0) {
        free(meta); snprintf(err, errcap, "failed to read PFS0 metadata"); return -6;
    }
    const char *st = (const char *)(meta + (size_t)nf * 24);
    uint64_t data_base = pfs_base + 16 + meta_sz;
    uint8_t *chunk = malloc(65536);
    if (!chunk) { free(meta); snprintf(err, errcap, "OOM chunk"); return -7; }

    int rc = 0;
    for (uint32_t i = 0; i < nf && rc == 0; i++) {
        if (cancel_flag && *cancel_flag) { rc = -99; snprintf(err, errcap, "cancelled"); break; }
        uint64_t foff = rd64(meta + i * 24);
        uint64_t fsz = rd64(meta + i * 24 + 8);
        uint32_t noff = rd32(meta + i * 24 + 16);
        if (noff >= stsz) { rc = -8; snprintf(err, errcap, "PFS0 name OOB"); break; }
        char name[128];
        size_t nl = strnlen(st + noff, stsz - noff);
        if (nl == 0 || nl >= sizeof(name) || strchr(st + noff, '/')) {
            rc = -9; snprintf(err, errcap, "unsafe PFS0 entry name"); break;
        }
        memcpy(name, st + noff, nl); name[nl] = '\0';
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", out_dir, name);
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) { rc = -10; snprintf(err, errcap, "cannot create %s", path); break; }
        uint64_t done = 0;
        while (done < fsz) {
            if (cancel_flag && *cancel_flag) { rc = -99; snprintf(err, errcap, "cancelled"); break; }
            size_t take = (fsz - done) < 65536 ? (size_t)(fsz - done) : 65536;
            if (nca_read_section_plain_ctr(nca, sec_idx, data_base + foff + done, chunk, take) != 0 ||
                write_all(fd, chunk, take) != 0) {
                rc = -11; snprintf(err, errcap, "write failed on %s", path); break;
            }
            done += take;
        }
        close(fd);
    }
    free(chunk);
    free(meta);
    return rc;
}

/* ---------------- BKTR (AesCtrEx) Patch Overlay ---------------- */
int nx_bktr_open(const nx_nca *base_nca, int base_sec, const nx_nca *upd_nca, int upd_sec,
                 nx_bktr *bktr, char *err, size_t errcap) {
    memset(bktr, 0, sizeof *bktr);
    const nx_nca_section *us = &upd_nca->sec[upd_sec];
    if (!base_nca->sec[base_sec].exists || !us->exists || us->enc_type != 4) {
        snprintf(err, errcap, "Update NCA section %d is not BKTR (enc_type=%u)", upd_sec, us->enc_type);
        return -1;
    }
    bktr->base_nca = base_nca;
    bktr->base_sec = base_sec;
    bktr->upd_nca = upd_nca;
    bktr->upd_sec = upd_sec;

    /* 1. Read Relocation BKTR table (decrypted with plain Update AesCtr) */
    if (us->reloc_sz < 0x4000 || us->reloc_sz > (32u << 20)) {
        snprintf(err, errcap, "invalid BKTR relocation size %llu", (unsigned long long)us->reloc_sz);
        return -2;
    }
    uint8_t *rbuf = malloc((size_t)us->reloc_sz);
    if (!rbuf) { snprintf(err, errcap, "OOM BKTR reloc"); return -3; }
    if (nca_read_section_plain_ctr(upd_nca, upd_sec, us->reloc_off, rbuf, (size_t)us->reloc_sz) != 0) {
        free(rbuf); snprintf(err, errcap, "failed to read BKTR relocation table"); return -4;
    }
    uint32_t r_buckets = rd32(rbuf + 4);
    bktr->total_size = rd64(rbuf + 8);
    if (r_buckets == 0 || (uint64_t)(r_buckets + 1) * 0x4000 > us->reloc_sz) {
        free(rbuf); snprintf(err, errcap, "corrupt BKTR reloc bucket count %u", r_buckets); return -5;
    }
    uint32_t total_reloc = 0;
    for (uint32_t b = 0; b < r_buckets; b++) {
        total_reloc += rd32(rbuf + (size_t)(b + 1) * 0x4000 + 4);
    }
    bktr->reloc = calloc(total_reloc ? total_reloc : 1, sizeof *bktr->reloc);
    if (!bktr->reloc) { free(rbuf); snprintf(err, errcap, "OOM reloc entries"); return -6; }
    uint32_t ri = 0;
    for (uint32_t b = 0; b < r_buckets; b++) {
        const uint8_t *bk = rbuf + (size_t)(b + 1) * 0x4000;
        uint32_t cnt = rd32(bk + 4);
        for (uint32_t j = 0; j < cnt; j++) {
            const uint8_t *e = bk + 16 + j * 20;
            bktr->reloc[ri].virt_offset = rd64(e + 0);
            bktr->reloc[ri].phys_offset = rd64(e + 8);
            bktr->reloc[ri].is_patch    = rd32(e + 16);
            ri++;
        }
    }
    bktr->reloc_count = ri;
    free(rbuf);

    /* 2. Read Encryption (AesCtrEx) BKTR table */
    if (us->enc_sz < 0x4000 || us->enc_sz > (32u << 20)) {
        nx_bktr_close(bktr); snprintf(err, errcap, "invalid BKTR encryption size"); return -7;
    }
    uint8_t *ebuf = malloc((size_t)us->enc_sz);
    if (!ebuf) { nx_bktr_close(bktr); snprintf(err, errcap, "OOM BKTR enc"); return -8; }
    if (nca_read_section_plain_ctr(upd_nca, upd_sec, us->enc_off, ebuf, (size_t)us->enc_sz) != 0) {
        free(ebuf); nx_bktr_close(bktr); snprintf(err, errcap, "failed to read BKTR encryption table"); return -9;
    }
    uint32_t e_buckets = rd32(ebuf + 4);
    if (e_buckets == 0 || (uint64_t)(e_buckets + 1) * 0x4000 > us->enc_sz) {
        free(ebuf); nx_bktr_close(bktr); snprintf(err, errcap, "corrupt BKTR enc bucket count"); return -10;
    }
    uint32_t total_enc = 0;
    for (uint32_t b = 0; b < e_buckets; b++) {
        total_enc += rd32(ebuf + (size_t)(b + 1) * 0x4000 + 4);
    }
    bktr->enc = calloc(total_enc ? total_enc : 1, sizeof *bktr->enc);
    if (!bktr->enc) { free(ebuf); nx_bktr_close(bktr); snprintf(err, errcap, "OOM enc entries"); return -11; }
    uint32_t ei = 0;
    for (uint32_t b = 0; b < e_buckets; b++) {
        const uint8_t *bk = ebuf + (size_t)(b + 1) * 0x4000;
        uint32_t cnt = rd32(bk + 4);
        for (uint32_t j = 0; j < cnt; j++) {
            const uint8_t *e = bk + 16 + j * 16;
            bktr->enc[ei].offset  = rd64(e + 0);
            bktr->enc[ei].ctr_val = rd32(e + 12);
            ei++;
        }
    }
    bktr->enc_count = ei;
    free(ebuf);
    return 0;
}

void nx_bktr_close(nx_bktr *bktr) {
    if (bktr) { free(bktr->reloc); free(bktr->enc); memset(bktr, 0, sizeof *bktr); }
}

static uint32_t bsearch_reloc(const nx_bktr *b, uint64_t voff) {
    uint32_t lo = 0, hi = b->reloc_count;
    while (lo + 1 < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (b->reloc[mid].virt_offset <= voff) lo = mid;
        else hi = mid;
    }
    return lo;
}

static uint32_t bsearch_enc(const nx_bktr *b, uint64_t uoff) {
    uint32_t lo = 0, hi = b->enc_count;
    while (lo + 1 < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (b->enc[mid].offset <= uoff) lo = mid;
        else hi = mid;
    }
    return lo;
}

static int bktr_read_upd_ctrex(const nx_bktr *b, uint64_t sec_off, uint8_t *dst, size_t len) {
    const nx_nca *u = b->upd_nca;
    const nx_nca_section *us = &u->sec[b->upd_sec];
    size_t done = 0;
    while (done < len) {
        uint64_t cur = sec_off + done;
        uint32_t ei = bsearch_enc(b, cur);
        uint64_t next_off = (ei + 1 < b->enc_count) ? b->enc[ei + 1].offset : (us->media_end - us->media_start);
        uint64_t avail = next_off > cur ? (next_off - cur) : 0;
        if (avail == 0) return -1;
        size_t take = (len - done) < avail ? (len - done) : (size_t)avail;
        uint64_t nca_off = us->media_start + cur;
        if (pread_all(u->fd, dst + done, take, u->nca_offset + nca_off) != 0) return -2;
        uint32_t ctr_val = b->enc[ei].ctr_val;
        uint8_t nonce[8];
        make_bktr_ctr_nonce(us->section_ctr, ctr_val, nonce);
        nx_aes128_ctr_xor(&u->body_aes, nonce, nca_off, dst + done, take);
        done += take;
    }
    return 0;
}

static int bktr_read(const nx_bktr *b, uint64_t voff, uint8_t *dst, size_t len) {
    size_t done = 0;
    while (done < len) {
        uint64_t cur = voff + done;
        uint32_t ri = bsearch_reloc(b, cur);
        uint64_t next_v = (ri + 1 < b->reloc_count) ? b->reloc[ri + 1].virt_offset : b->total_size;
        uint64_t avail = next_v > cur ? (next_v - cur) : 0;
        if (avail == 0) return -1;
        size_t take = (len - done) < avail ? (len - done) : (size_t)avail;
        uint64_t poff = b->reloc[ri].phys_offset + (cur - b->reloc[ri].virt_offset);
        if (b->reloc[ri].is_patch) {
            if (bktr_read_upd_ctrex(b, poff, dst + done, take) != 0) return -2;
        } else {
            if (nca_read_section_plain_ctr(b->base_nca, b->base_sec, poff, dst + done, take) != 0) return -3;
        }
        done += take;
    }
    return 0;
}

/* ---------------- Generic Streaming RomFS Extractor (plain NCA or BKTR) ---------------- */
typedef struct {
    int is_bktr;
    const nx_nca *nca;
    int sec_idx;
    const nx_bktr *bktr;
    uint64_t romfs_base; /* IVFC level 5 offset relative to section/BKTR start */
} romfs_src;

static int romfs_read(const romfs_src *src, uint64_t off, uint8_t *dst, size_t len) {
    if (src->is_bktr) return bktr_read(src->bktr, src->romfs_base + off, dst, len);
    return nca_read_section_plain_ctr(src->nca, src->sec_idx, src->romfs_base + off, dst, len);
}

typedef struct {
    uint32_t dir_off;
    char path[1024];
} dir_stack_item;

static int extract_romfs_common(const romfs_src *src, const char *out_dir,
                                nx_progress_fn cb, void *cb_user, volatile int *cancel_flag,
                                uint64_t *out_files, uint64_t *out_bytes, char *err, size_t errcap) {
    if (out_files) *out_files = 0;
    if (out_bytes) *out_bytes = 0;
    if (mkdir_p(out_dir) != 0) { snprintf(err, errcap, "cannot create RomFS dir: %s", out_dir); return -1; }

    uint8_t hdr[0x50];
    if (romfs_read(src, 0, hdr, 0x50) != 0) { snprintf(err, errcap, "failed to read RomFS header"); return -2; }
    uint64_t hdr_sz = rd64(hdr + 0x00);
    if (hdr_sz != 0x50) { snprintf(err, errcap, "invalid RomFS header size %#llx (bad key or BKTR)", (unsigned long long)hdr_sz); return -3; }

    uint64_t dir_meta_off  = rd64(hdr + 0x18);
    uint64_t dir_meta_sz   = rd64(hdr + 0x20);
    uint64_t file_meta_off = rd64(hdr + 0x38);
    uint64_t file_meta_sz  = rd64(hdr + 0x40);
    uint64_t data_off      = rd64(hdr + 0x48);

    if (dir_meta_sz > (32u << 20) || file_meta_sz > (32u << 20)) {
        snprintf(err, errcap, "RomFS metadata tables too large (dir=%llu file=%llu)",
                 (unsigned long long)dir_meta_sz, (unsigned long long)file_meta_sz);
        return -4;
    }
    uint8_t *dmeta = malloc((size_t)(dir_meta_sz ? dir_meta_sz : 1));
    uint8_t *fmeta = malloc((size_t)(file_meta_sz ? file_meta_sz : 1));
    uint8_t *chunk = malloc(65536);
    if (!dmeta || !fmeta || !chunk) {
        free(dmeta); free(fmeta); free(chunk); snprintf(err, errcap, "OOM RomFS metadata"); return -5;
    }
    if ((dir_meta_sz && romfs_read(src, dir_meta_off, dmeta, (size_t)dir_meta_sz) != 0) ||
        (file_meta_sz && romfs_read(src, file_meta_off, fmeta, (size_t)file_meta_sz) != 0)) {
        free(dmeta); free(fmeta); free(chunk); snprintf(err, errcap, "failed reading RomFS metadata"); return -6;
    }

    /* Pass 1: count total files & total bytes for accurate real-time progress */
    uint64_t total_files = 0, total_bytes = 0;
    {
        size_t stk_cap = 256, stk_n = 0;
        uint32_t *stk = malloc(stk_cap * sizeof(uint32_t));
        if (stk && dir_meta_sz >= 0x18) stk[stk_n++] = 0;
        while (stk && stk_n > 0) {
            uint32_t doff = stk[--stk_n];
            if ((uint64_t)doff + 0x18 > dir_meta_sz) continue;
            uint32_t sib = rd32(dmeta + doff + 0x04);
            uint32_t child = rd32(dmeta + doff + 0x08);
            uint32_t foff = rd32(dmeta + doff + 0x0C);
            if (sib != 0xFFFFFFFFu && doff != 0) {
                if (stk_n == stk_cap) { stk_cap *= 2; stk = realloc(stk, stk_cap * sizeof(uint32_t)); }
                if (stk) stk[stk_n++] = sib;
            }
            if (child != 0xFFFFFFFFu) {
                if (stk_n == stk_cap) { stk_cap *= 2; stk = realloc(stk, stk_cap * sizeof(uint32_t)); }
                if (stk) stk[stk_n++] = child;
            }
            while (foff != 0xFFFFFFFFu && (uint64_t)foff + 0x20 <= file_meta_sz) {
                total_files++;
                total_bytes += rd64(fmeta + foff + 0x10);
                foff = rd32(fmeta + foff + 0x04);
            }
        }
        free(stk);
    }

    /* Pass 2: iterative DFS extraction with 64 KiB streaming buffer */
    size_t stk_cap = 256, stk_n = 0;
    dir_stack_item *stk = malloc(stk_cap * sizeof *stk);
    if (!stk) { free(dmeta); free(fmeta); free(chunk); snprintf(err, errcap, "OOM dir stack"); return -7; }

    int rc = 0;
    uint64_t files_done = 0, bytes_done = 0;
    if (dir_meta_sz >= 0x18) {
        stk[0].dir_off = 0;
        snprintf(stk[0].path, sizeof stk[0].path, "%s", out_dir);
        stk_n = 1;
    }

    while (stk_n > 0 && rc == 0) {
        if (cancel_flag && *cancel_flag) { rc = -99; snprintf(err, errcap, "extraction cancelled"); break; }
        dir_stack_item cur = stk[--stk_n];
        if ((uint64_t)cur.dir_off + 0x18 > dir_meta_sz) { rc = -8; snprintf(err, errcap, "dir entry OOB"); break; }

        uint32_t child = rd32(dmeta + cur.dir_off + 0x08);
        uint32_t foff  = rd32(dmeta + cur.dir_off + 0x0C);

        if (mkdir_p(cur.path) != 0) { rc = -9; snprintf(err, errcap, "mkdir failed: %s", cur.path); break; }

        /* Push all child directories onto stack */
        uint32_t c = child;
        while (c != 0xFFFFFFFFu) {
            if ((uint64_t)c + 0x18 > dir_meta_sz) { rc = -10; snprintf(err, errcap, "child dir OOB"); break; }
            uint32_t nlen = rd32(dmeta + c + 0x14);
            if ((uint64_t)c + 0x18 + nlen > dir_meta_sz || nlen >= 256) {
                rc = -11; snprintf(err, errcap, "dir name OOB"); break;
            }
            if (stk_n == stk_cap) {
                stk_cap *= 2;
                dir_stack_item *ns = realloc(stk, stk_cap * sizeof *stk);
                if (!ns) { rc = -12; snprintf(err, errcap, "OOM growing stack"); break; }
                stk = ns;
            }
            stk[stk_n].dir_off = c;
            int wn = snprintf(stk[stk_n].path, sizeof stk[stk_n].path, "%s/%.*s", cur.path, (int)nlen, (const char *)(dmeta + c + 0x18));
            if (wn <= 0 || (size_t)wn >= sizeof stk[stk_n].path) { rc = -13; snprintf(err, errcap, "path too long"); break; }
            stk_n++;
            c = rd32(dmeta + c + 0x04);
        }
        if (rc != 0) break;

        /* Stream all files in this directory */
        while (foff != 0xFFFFFFFFu && rc == 0) {
            if (cancel_flag && *cancel_flag) { rc = -99; snprintf(err, errcap, "extraction cancelled"); break; }
            if ((uint64_t)foff + 0x20 > file_meta_sz) { rc = -14; snprintf(err, errcap, "file entry OOB"); break; }
            uint32_t next_f = rd32(fmeta + foff + 0x04);
            uint64_t f_off  = rd64(fmeta + foff + 0x08);
            uint64_t f_sz   = rd64(fmeta + foff + 0x10);
            uint32_t nlen   = rd32(fmeta + foff + 0x1C);
            if ((uint64_t)foff + 0x20 + nlen > file_meta_sz || nlen >= 256) {
                rc = -15; snprintf(err, errcap, "file name OOB"); break;
            }
            char fpath[1280];
            int wn = snprintf(fpath, sizeof fpath, "%s/%.*s", cur.path, (int)nlen, (const char *)(fmeta + foff + 0x20));
            if (wn <= 0 || (size_t)wn >= sizeof fpath) { rc = -16; snprintf(err, errcap, "file path too long"); break; }

            int fd = open(fpath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd < 0) { rc = -17; snprintf(err, errcap, "cannot create file: %s", fpath); break; }
            uint64_t rem = f_sz, pos = 0;
            while (rem > 0) {
                if (cancel_flag && *cancel_flag) { rc = -99; snprintf(err, errcap, "extraction cancelled"); break; }
                size_t take = rem < 65536 ? (size_t)rem : 65536;
                if (romfs_read(src, data_off + f_off + pos, chunk, take) != 0) {
                    rc = -18; snprintf(err, errcap, "RomFS read failed: %s", fpath); break;
                }
                if (write_all(fd, chunk, take) != 0) {
                    rc = -19; snprintf(err, errcap, "disk write failed: %s (errno=%d)", fpath, errno); break;
                }
                pos += take; rem -= take; bytes_done += take;
            }
            close(fd);
            if (rc != 0) break;
            files_done++;
            if (cb && ((files_done & 63u) == 0 || files_done == total_files)) {
                cb(cb_user, "romfs", files_done, total_files, bytes_done, total_bytes);
            }
            foff = next_f;
        }
    }

    free(stk);
    free(dmeta);
    free(fmeta);
    free(chunk);
    if (rc == 0) {
        if (out_files) *out_files = files_done;
        if (out_bytes) *out_bytes = bytes_done;
    }
    return rc;
}

int nx_nca_extract_romfs(const nx_nca *nca, int sec_idx, const char *out_dir,
                         nx_progress_fn cb, void *cb_user, volatile int *cancel_flag,
                         uint64_t *out_files, uint64_t *out_bytes, char *err, size_t errcap) {
    if (!nca->sec[sec_idx].exists || nca->sec[sec_idx].fs_type != 0) {
        snprintf(err, errcap, "NCA section %d is not RomFS", sec_idx);
        return -1;
    }
    romfs_src src = { .is_bktr = 0, .nca = nca, .sec_idx = sec_idx, .bktr = NULL,
                      .romfs_base = nca->sec[sec_idx].data_offset };
    return extract_romfs_common(&src, out_dir, cb, cb_user, cancel_flag, out_files, out_bytes, err, errcap);
}

int nx_bktr_extract_romfs(const nx_bktr *bktr, const char *out_dir,
                          nx_progress_fn cb, void *cb_user, volatile int *cancel_flag,
                          uint64_t *out_files, uint64_t *out_bytes, char *err, size_t errcap) {
    romfs_src src = { .is_bktr = 1, .nca = NULL, .sec_idx = 0, .bktr = bktr,
                      .romfs_base = bktr->upd_nca->sec[bktr->upd_sec].data_offset };
    return extract_romfs_common(&src, out_dir, cb, cb_user, cancel_flag, out_files, out_bytes, err, errcap);
}
