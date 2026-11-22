#define _FILE_OFFSET_BITS 64
#include "nx_patch_stream.h"
#include "nx_crypto.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static inline uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline uint64_t rd64(const uint8_t *p) {
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

static int pread_exact(int fd, void *buf, size_t len, uint64_t off) {
    uint8_t *p = (uint8_t *)buf;
    size_t got = 0;
    while (got < len) {
        ssize_t r = pread(fd, p + got, len - got, (off_t)(off + got));
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    return 0;
}

static int write_exact(int fd, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    size_t put = 0;
    while (put < len) {
        ssize_t w = write(fd, p + put, len - put);
        if (w <= 0) return -1;
        put += (size_t)w;
    }
    return 0;
}

/* Streaming buffered reader over an fd range [base_off, base_off + limit) */
typedef struct {
    int fd;
    uint64_t base_off;
    uint64_t pos;
    uint64_t limit;
    uint8_t buf[65536];
    size_t bpos, blen;
} fd_reader;

static void fdr_init(fd_reader *r, int fd, uint64_t off, uint64_t len) {
    r->fd = fd; r->base_off = off; r->pos = 0; r->limit = len; r->bpos = 0; r->blen = 0;
}
static int fdr_fill(fd_reader *r) {
    if (r->pos >= r->limit) return 0;
    uint64_t rem = r->limit - r->pos;
    size_t want = rem < sizeof(r->buf) ? (size_t)rem : sizeof(r->buf);
    if (pread_exact(r->fd, r->buf, want, r->base_off + r->pos) != 0) return -1;
    r->pos += want;
    r->bpos = 0;
    r->blen = want;
    return (int)want;
}
static inline int fdr_getc(fd_reader *r) {
    if (r->bpos >= r->blen) {
        int n = fdr_fill(r);
        if (n <= 0) return -1;
    }
    return r->buf[r->bpos++];
}
static int fdr_read(fd_reader *r, uint8_t *dst, size_t n) {
    while (n > 0) {
        if (r->bpos >= r->blen) {
            if (fdr_fill(r) <= 0) return -1;
        }
        size_t avail = r->blen - r->bpos;
        size_t take = avail < n ? avail : n;
        memcpy(dst, r->buf + r->bpos, take);
        r->bpos += take; dst += take; n -= take;
    }
    return 0;
}

/* Streaming writer + base SHA-256 + sorted NXPT hunk overlay + output SHA-256 */
typedef struct {
    int out_fd;
    int pat_fd;
    uint64_t pat_hunk_pos;   /* current file offset in pat_fd for next hunk */
    uint32_t hunks_rem;
    uint64_t cur_hunk_off;
    uint32_t cur_hunk_len;
    uint32_t cur_hunk_done;
    int have_hunk;

    uint64_t out_pos;        /* current byte position in output image */
    uint64_t base_img_len;

    nx_sha256_ctx base_sha;
    nx_sha256_ctx out_sha;

    uint8_t wbuf[65536];
    size_t wlen;
    uint8_t hbuf[4096];      /* scratch for hunk bytes */
} patch_sink;

static int sink_flush(patch_sink *s) {
    if (!s->wlen) return 0;
    nx_sha256_update(&s->out_sha, s->wbuf, s->wlen);
    if (write_exact(s->out_fd, s->wbuf, s->wlen) != 0) return -1;
    s->wlen = 0;
    return 0;
}

static int sink_load_next_hunk(patch_sink *s) {
    if (s->have_hunk || s->hunks_rem == 0) return 0;
    uint8_t hh[8];
    if (pread_exact(s->pat_fd, hh, 8, s->pat_hunk_pos) != 0) return -1;
    s->pat_hunk_pos += 8;
    s->cur_hunk_off = rd32(hh);
    s->cur_hunk_len = rd32(hh + 4);
    s->cur_hunk_done = 0;
    s->hunks_rem--;
    s->have_hunk = 1;
    return 0;
}

/* Emit `len` base-image bytes (updates base_sha, applies overlapping sorted hunks, updates out_sha, writes to out_fd). */
static int sink_emit_base(patch_sink *s, const uint8_t *data, size_t len) {
    if (len == 0) return 0;
    nx_sha256_update(&s->base_sha, data, len);

    size_t p = 0;
    while (p < len) {
        if (!s->have_hunk && s->hunks_rem > 0) {
            if (sink_load_next_hunk(s) != 0) return -1;
        }
        size_t chunk = len - p;
        size_t wroom = sizeof(s->wbuf) - s->wlen;
        if (chunk > wroom) chunk = wroom;

        if (s->have_hunk) {
            uint64_t h_cur = s->cur_hunk_off + s->cur_hunk_done;
            uint64_t h_rem = s->cur_hunk_len - s->cur_hunk_done;
            if (s->out_pos < h_cur) {
                uint64_t until = h_cur - s->out_pos;
                if (chunk > until) chunk = (size_t)until;
                memcpy(s->wbuf + s->wlen, data + p, chunk);
                s->wlen += chunk;
                s->out_pos += chunk;
                p += chunk;
            } else if (s->out_pos == h_cur) {
                if (chunk > h_rem) chunk = (size_t)h_rem;
                if (chunk > sizeof(s->hbuf)) chunk = sizeof(s->hbuf);
                if (pread_exact(s->pat_fd, s->hbuf, chunk, s->pat_hunk_pos) != 0) return -1;
                s->pat_hunk_pos += chunk;
                memcpy(s->wbuf + s->wlen, s->hbuf, chunk);
                s->wlen += chunk;
                s->out_pos += chunk;
                s->cur_hunk_done += (uint32_t)chunk;
                p += chunk;
                if (s->cur_hunk_done == s->cur_hunk_len) s->have_hunk = 0;
            } else {
                return -2; /* unsorted or overlapping hunks */
            }
        } else {
            memcpy(s->wbuf + s->wlen, data + p, chunk);
            s->wlen += chunk;
            s->out_pos += chunk;
            p += chunk;
        }
        if (s->wlen == sizeof(s->wbuf)) {
            if (sink_flush(s) != 0) return -1;
        }
    }
    return 0;
}

static int sink_emit_zeros_base(patch_sink *s, uint64_t count) {
    static const uint8_t Z[4096] = {0};
    while (count > 0) {
        size_t take = count < sizeof(Z) ? (size_t)count : sizeof(Z);
        if (sink_emit_base(s, Z, take) != 0) return -1;
        count -= take;
    }
    return 0;
}

static int sink_emit_raw_out(patch_sink *s, const uint8_t *data, size_t len) {
    size_t p = 0;
    while (p < len) {
        size_t room = sizeof(s->wbuf) - s->wlen;
        size_t take = (len - p) < room ? (len - p) : room;
        memcpy(s->wbuf + s->wlen, data + p, take);
        s->wlen += take;
        s->out_pos += take;
        p += take;
        if (s->wlen == sizeof(s->wbuf)) {
            if (sink_flush(s) != 0) return -1;
        }
    }
    return 0;
}

static int sink_emit_zeros_out(patch_sink *s, uint64_t count) {
    static const uint8_t Z[4096] = {0};
    while (count > 0) {
        size_t take = count < sizeof(Z) ? (size_t)count : sizeof(Z);
        if (sink_emit_raw_out(s, Z, take) != 0) return -1;
        count -= take;
    }
    return 0;
}

/* Streaming LZ4 block decompressor using a 64 KiB ring window -> emits directly into patch_sink */
static int lz4_stream_to_sink(int nso_fd, uint64_t c_off, uint64_t c_len, uint64_t u_len, patch_sink *sink) {
    fd_reader r;
    fdr_init(&r, nso_fd, c_off, c_len);
    uint8_t ring[65536];
    uint8_t tmp[4096];
    size_t rpos = 0;
    uint64_t produced = 0;

    while (1) {
        int tok = fdr_getc(&r);
        if (tok < 0) break;
        size_t ll = (size_t)(tok >> 4);
        if (ll == 15) {
            int b;
            do { b = fdr_getc(&r); if (b < 0) return -1; ll += (size_t)b; } while (b == 255);
        }
        if (produced + ll > u_len) return -2;
        while (ll > 0) {
            size_t take = ll < sizeof(tmp) ? ll : sizeof(tmp);
            if (fdr_read(&r, tmp, take) != 0) return -3;
            if (sink_emit_base(sink, tmp, take) != 0) return -4;
            for (size_t i = 0; i < take; i++) ring[(rpos + i) & 0xFFFFu] = tmp[i];
            rpos = (rpos + take) & 0xFFFFu;
            produced += take;
            ll -= take;
        }
        if (produced == u_len) break;
        int lo = fdr_getc(&r), hi = fdr_getc(&r);
        if (lo < 0 || hi < 0) return -5;
        size_t off = (size_t)lo | ((size_t)hi << 8);
        if (off == 0 || (uint64_t)off > produced) return -6;
        size_t ml = (size_t)(tok & 0x0F) + 4;
        if ((tok & 0x0F) == 15) {
            int b;
            do { b = fdr_getc(&r); if (b < 0) return -7; ml += (size_t)b; } while (b == 255);
        }
        if (produced + ml > u_len) return -8;
        while (ml > 0) {
            size_t take = ml < sizeof(tmp) ? ml : sizeof(tmp);
            for (size_t i = 0; i < take; i++) {
                uint8_t v = ring[(rpos - off) & 0xFFFFu];
                ring[rpos] = v;
                rpos = (rpos + 1) & 0xFFFFu;
                tmp[i] = v;
            }
            if (sink_emit_base(sink, tmp, take) != 0) return -9;
            produced += take;
            ml -= take;
        }
    }
    return produced == u_len ? 0 : -10;
}

int nx_patch_apply_streaming(const char *raw_nso_path, const char *nxpatch_path,
                             const char *out_path, char *err, size_t errcap) {
    int nso_fd = open(raw_nso_path, O_RDONLY);
    if (nso_fd < 0) { snprintf(err, errcap, "cannot open NSO: %s", raw_nso_path); return -1; }
    int pat_fd = open(nxpatch_path, O_RDONLY);
    if (pat_fd < 0) { close(nso_fd); snprintf(err, errcap, "cannot open nxpatch: %s", nxpatch_path); return -2; }

    struct stat nso_st, pat_st;
    if (fstat(nso_fd, &nso_st) != 0 || fstat(pat_fd, &pat_st) != 0) {
        close(nso_fd); close(pat_fd); snprintf(err, errcap, "fstat failed"); return -3;
    }
    uint64_t nso_len = (uint64_t)nso_st.st_size;
    uint64_t pat_len = (uint64_t)pat_st.st_size;

    uint8_t hdr[160];
    if (pat_len < 160 || pread_exact(pat_fd, hdr, 160, 0) != 0 || memcmp(hdr, "NXPT", 4) != 0 || rd32(hdr + 4) != 1) {
        close(nso_fd); close(pat_fd); snprintf(err, errcap, "invalid NXPT header"); return -4;
    }
    uint32_t pflags = rd32(hdr + 8);
    uint32_t num_hunks = rd32(hdr + 12);
    const uint8_t *exp_bid = hdr + 16;
    const uint8_t *exp_raw_sha = hdr + 32;
    const uint8_t *exp_base_sha = hdr + 64;
    const uint8_t *exp_out_sha = hdr + 96;
    uint64_t base_img_len = rd64(hdr + 128);
    uint64_t out_total_len = rd64(hdr + 136);
    uint64_t tail_off = rd64(hdr + 144);
    uint64_t tail_len = rd64(hdr + 152);

    uint8_t nso_hdr[0x100];
    if (nso_len < 0x100 || pread_exact(nso_fd, nso_hdr, 0x100, 0) != 0 || memcmp(nso_hdr, "NSO0", 4) != 0) {
        close(nso_fd); close(pat_fd); snprintf(err, errcap, "invalid NSO0 header"); return -5;
    }
    if (memcmp(nso_hdr + 0x40, exp_bid, 16) != 0) {
        close(nso_fd); close(pat_fd); snprintf(err, errcap, "NSO Build ID mismatch"); return -6;
    }

    /* Streaming SHA-256 verification of raw NSO */
    {
        nx_sha256_ctx sc;
        nx_sha256_init(&sc);
        uint8_t buf[65536];
        uint64_t off = 0;
        while (off < nso_len) {
            size_t take = (nso_len - off) < sizeof(buf) ? (size_t)(nso_len - off) : sizeof(buf);
            if (pread_exact(nso_fd, buf, take, off) != 0) {
                close(nso_fd); close(pat_fd); snprintf(err, errcap, "pread NSO failed"); return -7;
            }
            nx_sha256_update(&sc, buf, take);
            off += take;
        }
        uint8_t got[32];
        nx_sha256_final(&sc, got);
        if (memcmp(got, exp_raw_sha, 32) != 0) {
            close(nso_fd); close(pat_fd); snprintf(err, errcap, "raw NSO SHA-256 mismatch"); return -8;
        }
    }

    int far = (pflags & 1) != 0;
    uint32_t nso_flags = rd32(nso_hdr + 0x0C);
    uint32_t seg_fo[3], seg_mo[3], seg_sz[3], seg_csz[3];
    for (int i = 0; i < 3; i++) {
        seg_fo[i]  = rd32(nso_hdr + 0x10 + i * 0x10);
        seg_mo[i]  = rd32(nso_hdr + 0x14 + i * 0x10);
        seg_sz[i]  = rd32(nso_hdr + 0x18 + i * 0x10);
        seg_csz[i] = rd32(nso_hdr + 0x60 + i * 4);
    }
    uint32_t bss = rd32(nso_hdr + 0x3C);
    uint64_t calc_base = (uint64_t)seg_mo[2] + seg_sz[2] + (far ? 0 : bss);
    if (calc_base != base_img_len || out_total_len < base_img_len || tail_off < base_img_len ||
        tail_off > out_total_len || tail_off + tail_len != out_total_len || tail_len > pat_len - 160) {
        close(nso_fd); close(pat_fd); snprintf(err, errcap, "NXPT layout mismatch"); return -9;
    }

    int out_fd = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (out_fd < 0) { close(nso_fd); close(pat_fd); snprintf(err, errcap, "cannot create output: %s", out_path); return -10; }

    patch_sink *sink = calloc(1, sizeof *sink);
    if (!sink) { close(out_fd); close(nso_fd); close(pat_fd); snprintf(err, errcap, "OOM sink"); return -11; }
    sink->out_fd = out_fd;
    sink->pat_fd = pat_fd;
    sink->pat_hunk_pos = 160;
    sink->hunks_rem = num_hunks;
    sink->base_img_len = base_img_len;
    nx_sha256_init(&sink->base_sha);
    nx_sha256_init(&sink->out_sha);

    int rc = 0;
    for (int i = 0; i < 3 && rc == 0; i++) {
        if ((uint64_t)seg_mo[i] < sink->out_pos) { rc = -12; snprintf(err, errcap, "overlapping NSO segment %d", i); break; }
        if ((uint64_t)seg_mo[i] > sink->out_pos) {
            if (sink_emit_zeros_base(sink, (uint64_t)seg_mo[i] - sink->out_pos) != 0) { rc = -13; snprintf(err, errcap, "zero gap emit failed"); break; }
        }
        if ((nso_flags >> i) & 1) {
            if (lz4_stream_to_sink(nso_fd, seg_fo[i], seg_csz[i], seg_sz[i], sink) != 0) {
                rc = -14; snprintf(err, errcap, "streaming LZ4 segment %d failed", i); break;
            }
        } else {
            uint8_t buf[65536];
            uint64_t done = 0;
            while (done < seg_sz[i]) {
                size_t take = (seg_sz[i] - done) < sizeof(buf) ? (size_t)(seg_sz[i] - done) : sizeof(buf);
                if (pread_exact(nso_fd, buf, take, seg_fo[i] + done) != 0 || sink_emit_base(sink, buf, take) != 0) {
                    rc = -15; snprintf(err, errcap, "raw segment %d copy failed", i); break;
                }
                done += take;
            }
        }
    }
    /* Remaining BSS zeros inside base_img_len */
    if (rc == 0 && sink->out_pos < base_img_len) {
        if (sink_emit_zeros_base(sink, base_img_len - sink->out_pos) != 0) { rc = -16; snprintf(err, errcap, "BSS zero emit failed"); }
    }
    /* Verify decompressed base image SHA-256 and that all hunks were consumed */
    if (rc == 0) {
        uint8_t got_base[32];
        nx_sha256_final(&sink->base_sha, got_base);
        if (memcmp(got_base, exp_base_sha, 32) != 0) {
            rc = -17; snprintf(err, errcap, "decompressed base image SHA-256 mismatch");
        } else if (sink->have_hunk || sink->hunks_rem != 0) {
            rc = -18; snprintf(err, errcap, "unconsumed hunks remaining (%u)", sink->hunks_rem);
        } else if (sink->pat_hunk_pos + tail_len != pat_len) {
            rc = -19; snprintf(err, errcap, "tail offset mismatch in nxpatch");
        }
    }
    /* Pad zeros between base_img_len and tail_off, then stream tail from pat_fd */
    if (rc == 0 && tail_off > base_img_len) {
        if (sink_emit_zeros_out(sink, tail_off - base_img_len) != 0) { rc = -20; snprintf(err, errcap, "tail pad emit failed"); }
    }
    if (rc == 0 && tail_len > 0) {
        uint8_t buf[65536];
        uint64_t done = 0;
        while (done < tail_len) {
            size_t take = (tail_len - done) < sizeof(buf) ? (size_t)(tail_len - done) : sizeof(buf);
            if (pread_exact(pat_fd, buf, take, sink->pat_hunk_pos + done) != 0 || sink_emit_raw_out(sink, buf, take) != 0) {
                rc = -21; snprintf(err, errcap, "tail stream failed"); break;
            }
            done += take;
        }
    }
    if (rc == 0 && sink_flush(sink) != 0) { rc = -22; snprintf(err, errcap, "final flush failed"); }
    if (rc == 0) {
        uint8_t got_out[32];
        nx_sha256_final(&sink->out_sha, got_out);
        if (memcmp(got_out, exp_out_sha, 32) != 0) {
            rc = -23; snprintf(err, errcap, "patched output SHA-256 verification failed");
        }
    }

    free(sink);
    close(out_fd);
    close(nso_fd);
    close(pat_fd);
    if (rc != 0) unlink(out_path);
    return rc;
}
