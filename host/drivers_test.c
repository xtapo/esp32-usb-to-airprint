// Host-side unit and round-trip tests for JBIG1, HP ZjStream/XQX,
// Canon LBP CAPT/Hi-SCoA, and Brother Laser Mode 1030 drivers.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jbig1_enc.h"
#include "mono_raster.h"
#include "printer_driver.h"

// Internal encoder functions exposed for round-trip unit testing
size_t canon_hiscoa_compress_band(const uint8_t *band, unsigned line_size, unsigned nlines, int eob_last, uint8_t *out,
                                  size_t out_cap);
size_t brother_encode_line(const uint8_t *line, const uint8_t *ref, int line_bytes, uint8_t *out);

static int failures;
#define CHECK(cond, ...)                                                                           \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                                   \
            fprintf(stderr, __VA_ARGS__);                                                          \
            fprintf(stderr, "\n");                                                                 \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

typedef struct {
    uint8_t *buf;
    size_t len;
    size_t cap;
} mem_buf_t;

static int mem_write(void *ctx, const uint8_t *data, size_t len)
{
    mem_buf_t *m = ctx;
    if (m->len + len > m->cap) {
        size_t ncap = m->cap ? m->cap * 2 : 65536;
        while (ncap < m->len + len)
            ncap *= 2;
        uint8_t *nb = realloc(m->buf, ncap);
        if (!nb)
            return -1;
        m->buf = nb;
        m->cap = ncap;
    }
    memcpy(m->buf + m->len, data, len);
    m->len += len;
    return 0;
}

static int jbig_mem_write(void *ctx, const uint8_t *data, size_t len)
{
    return mem_write(ctx, data, len);
}

// ===========================================================================
// Clean-room ITU-T T.82 2-line-template QM-coder decoder for round-trip tests

static const uint16_t DEC_LSZ[113] = {
    0x5a1d, 0x2586, 0x1114, 0x080b, 0x03d8, 0x01da, 0x00e5, 0x006f,
    0x0036, 0x001a, 0x000d, 0x0006, 0x0003, 0x0001, 0x5a7f, 0x3f25,
    0x2cf2, 0x207c, 0x17b9, 0x1182, 0x0cef, 0x09a1, 0x072f, 0x055c,
    0x0406, 0x0303, 0x0240, 0x01b1, 0x0144, 0x00f5, 0x00b7, 0x008a,
    0x0068, 0x004e, 0x003b, 0x002c, 0x5ae1, 0x484c, 0x3a0d, 0x2ef1,
    0x261f, 0x1f33, 0x19a8, 0x1518, 0x1177, 0x0e74, 0x0bfb, 0x09f8,
    0x0861, 0x0706, 0x05cd, 0x04de, 0x040f, 0x0363, 0x02d4, 0x025c,
    0x01f8, 0x01a4, 0x0160, 0x0125, 0x00f6, 0x00cb, 0x00ab, 0x008f,
    0x5b12, 0x4d04, 0x412c, 0x37d8, 0x2fe8, 0x293c, 0x2379, 0x1edf,
    0x1aa9, 0x174e, 0x1424, 0x119c, 0x0f6b, 0x0d51, 0x0bb6, 0x0a40,
    0x5832, 0x4d1c, 0x438e, 0x3bdd, 0x34ee, 0x2eae, 0x299a, 0x2516,
    0x5570, 0x4ca9, 0x44d9, 0x3e22, 0x3824, 0x32b4, 0x2e17, 0x56a8,
    0x4f46, 0x47e5, 0x41cf, 0x3c3d, 0x375e, 0x5231, 0x4c0f, 0x4639,
    0x415e, 0x5627, 0x50e7, 0x4b85, 0x5597, 0x504f, 0x5a10, 0x5522,
    0x59eb,
};

static const uint8_t DEC_NMPS[113] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 13, 15, 16,
    17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32,
    33, 34, 35, 9, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48,
    49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 32,
    65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79, 48,
    81, 82, 83, 84, 85, 86, 87, 71, 89, 90, 91, 92, 93, 94, 86, 96,
    97, 98, 99, 100, 93, 102, 103, 104, 99, 106, 107, 103, 109, 107, 111, 109,
    111,
};

static const uint8_t DEC_NLPS[113] = {
    129, 14, 16, 18, 20, 23, 25, 28, 30, 33, 35, 9, 10, 12, 143, 36,
    38, 39, 40, 42, 43, 45, 46, 48, 49, 51, 52, 54, 56, 57, 59, 60,
    62, 63, 32, 33, 165, 64, 65, 67, 68, 69, 70, 72, 73, 74, 75, 77,
    78, 79, 48, 50, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 61, 61,
    193, 80, 81, 82, 83, 84, 86, 87, 87, 72, 72, 74, 74, 75, 77, 77,
    208, 88, 89, 90, 91, 92, 93, 86, 216, 95, 96, 97, 99, 99, 93, 223,
    101, 102, 103, 104, 99, 105, 106, 107, 103, 233, 108, 109, 110, 111, 238, 112,
    240,
};

typedef struct {
    const uint8_t *ptr;
    const uint8_t *end;
    uint32_t c;
    uint32_t a;
    int ct;
    int startup;
    uint8_t ctx[1024];
} qm_dec_t;

static void qm_dec_init(qm_dec_t *d, const uint8_t *src, const uint8_t *end, int reuse_ctx)
{
    d->ptr = src;
    d->end = end;
    d->c = 0;
    d->a = 1u;
    d->ct = 0;
    d->startup = 1;
    if (!reuse_ctx)
        memset(d->ctx, 0, sizeof(d->ctx));
}

static int qm_dec_sym(qm_dec_t *d, uint32_t cx)
{
    while (d->a < 0x8000u || d->startup) {
        while (d->ct <= 8 && d->ct >= 0) {
            if (d->ptr >= d->end) {
                d->ct = -1;
            } else if (*d->ptr == 0xffu) {
                if (d->ptr + 1 < d->end && d->ptr[1] == 0x00u) {
                    d->c |= 0xffu << (8 - d->ct);
                    d->ct += 8;
                    d->ptr += 2;
                } else {
                    d->ct = -1;
                }
            } else {
                d->c |= (uint32_t)(*d->ptr++) << (8 - d->ct);
                d->ct += 8;
            }
        }
        d->c <<= 1;
        d->a <<= 1;
        if (d->ct >= 0)
            d->ct--;
        if (d->a == 0x10000u)
            d->startup = 0;
    }

    uint8_t *st = &d->ctx[cx];
    uint8_t ss = *st & 0x7fu;
    uint32_t lsz = DEC_LSZ[ss];
    int mps = *st >> 7;
    int pix;

    d->a -= lsz;
    if ((d->c >> 16) < d->a) {
        if (d->a & 0xffff8000u)
            return mps;
        if (d->a < lsz) {
            pix = 1 - mps;
            *st = (uint8_t)((*st & 0x80u) ^ DEC_NLPS[ss]);
        } else {
            pix = mps;
            *st = (uint8_t)((*st & 0x80u) | DEC_NMPS[ss]);
        }
    } else {
        d->c -= d->a << 16;
        if (d->a < lsz) {
            d->a = lsz;
            pix = mps;
            *st = (uint8_t)((*st & 0x80u) | DEC_NMPS[ss]);
        } else {
            d->a = lsz;
            pix = 1 - mps;
            *st = (uint8_t)((*st & 0x80u) ^ DEC_NLPS[ss]);
        }
    }
    return pix;
}

static bool jbig1_roundtrip_check(uint32_t w, uint32_t h, uint32_t l0, const uint8_t *img, const char *name)
{
    uint32_t lb = (w + 7u) / 8u;
    mem_buf_t sde = {0};
    jbig1_enc_t enc;
    if (jbig1_enc_init(&enc, w, h, l0, jbig_mem_write, &sde) != 0)
        return false;
    uint8_t bih[20];
    jbig1_enc_get_bih(&enc, bih);
    for (uint32_t y = 0; y < h; y++) {
        if (jbig1_enc_add_line(&enc, img + (size_t)y * lb) != 0) {
            jbig1_enc_free(&enc);
            free(sde.buf);
            return false;
        }
    }
    if (jbig1_enc_finish(&enc) != 0) {
        jbig1_enc_free(&enc);
        free(sde.buf);
        return false;
    }
    jbig1_enc_free(&enc);

    CHECK(bih[2] == 1 && bih[19] == JBIG1_OPT_LRLTWO, "%s: invalid BIH header", name);
    CHECK(sde.len >= 2 && sde.buf[sde.len - 2] == 0xff && sde.buf[sde.len - 1] == 0x02,
          "%s: missing trailing SDNORM", name);

    uint8_t *dec = calloc(h, lb);
    uint8_t *zeros = calloc(1, lb);
    qm_dec_t qd;
    const uint8_t *p = sde.buf;
    const uint8_t *end = sde.buf + sde.len;

    for (uint32_t y0 = 0; y0 < h; y0 += l0) {
        // Locate next SDNORM marker for this stripe
        const uint8_t *seg_end = p;
        while (seg_end + 1 < end) {
            if (seg_end[0] == 0xffu) {
                if (seg_end[1] == 0x00u) {
                    seg_end += 2;
                    continue;
                }
                if (seg_end[1] == 0x02u)
                    break;
            }
            seg_end++;
        }
        qm_dec_init(&qd, p, seg_end, y0 > 0);
        uint32_t y1 = y0 + l0 < h ? y0 + l0 : h;
        for (uint32_t y = y0; y < y1; y++) {
            const uint8_t *prev = (y > 0) ? (dec + (size_t)(y - 1u) * lb) : zeros;
            uint8_t *cur = dec + (size_t)y * lb;
            uint32_t h1 = 0, h2 = (uint32_t)prev[0] << 8;
            for (uint32_t bi = 0; bi < lb; bi++) {
                h2 |= (bi + 1u < lb) ? prev[bi + 1u] : 0;
                uint32_t cnt = w - bi * 8u;
                if (cnt > 8u)
                    cnt = 8u;
                for (uint32_t b = 0; b < cnt; b++) {
                    h1 <<= 1;
                    h2 <<= 1;
                    uint32_t cx = ((h2 >> 10) & 0x3f0u) | ((h1 >> 9) & 0x00fu);
                    int pix = qm_dec_sym(&qd, cx);
                    if (pix) {
                        h1 |= 0x100u;
                        cur[bi] |= (uint8_t)(0x80u >> b);
                    }
                }
            }
        }
        p = (seg_end + 2 <= end) ? (seg_end + 2) : end;
    }

    bool ok = memcmp(dec, img, (size_t)h * lb) == 0;
    CHECK(ok, "%s: JBIG1 round-trip mismatch (%ux%u)", name, w, h);
    free(dec);
    free(zeros);
    free(sde.buf);
    return ok;
}

// ===========================================================================
// Hi-SCoA decoder for round-trip testing
typedef struct {
    const uint8_t *in;
    size_t in_bytes;
    size_t bitpos;
} hs_bit_t;

static int hs_get_bit(hs_bit_t *b)
{
    if (b->bitpos >= b->in_bytes * 8u)
        return -1;
    uint8_t x = b->in[b->bitpos >> 3] ^ 0x43u;
    int bit = (x >> (7u - (b->bitpos & 7u))) & 1u;
    b->bitpos++;
    return bit;
}

static uint32_t hs_get_bits(hs_bit_t *b, int n)
{
    uint32_t v = 0;
    for (int i = 0; i < n; i++)
        v = (v << 1) | (uint32_t)hs_get_bit(b);
    return v;
}

static unsigned hs_get_gamma(hs_bit_t *b)
{
    unsigned ord = 0;
    while (ord < 6 && hs_get_bit(b) == 1)
        ord++;
    if (ord == 6)
        return 0;
    if (ord == 0) {
        if (hs_get_bit(b) == 0)
            return 1;
        return hs_get_bit(b) ? 2 : 3;
    }
    uint32_t nbits = hs_get_bits(b, (int)(ord + 1u));
    return ((1u << (ord + 2u)) - 1u - nbits);
}

static void hiscoa_roundtrip_check_mode(const uint8_t *band, unsigned ls, unsigned nlines, int eob_last,
                                        const char *name)
{
    size_t cap = (size_t)ls * nlines * 2u + 64u;
    uint8_t *comp = malloc(cap);
    uint8_t *dec = calloc(nlines, ls);
    size_t clen = canon_hiscoa_compress_band(band, ls, nlines, eob_last, comp, cap);
    CHECK(clen > 0 && (clen % 4u) == 0, "%s: Hi-SCoA alignment/len %zu", name, clen);

    hs_bit_t br = {comp, clen, 0};
    uint8_t stash[16] = {0};
    unsigned nstash = 0;
    int origin[6] = {(int)ls, 0, (int)ls - 5, 4, 384, 12};
    size_t out_pos = 0;
    size_t total = (size_t)ls * nlines;
    unsigned prefix = 0;

    while (out_pos <= total) {
        if (br.bitpos >= br.in_bytes * 8u)
            break;
        int cmd = 0;
        while (cmd < 8 && hs_get_bit(&br) == 1)
            cmd++;
        if (cmd >= 8)
            break;
        if (cmd == 7) {
            uint32_t eob = hs_get_bits(&br, 2);
            CHECK(eob == (uint32_t)(eob_last ? 1 : 0), "%s: unexpected EOB %u", name, (unsigned)eob);
            break;
        }
        if (cmd == 1) {
            if (out_pos >= total)
                break;
            uint32_t idx = hs_get_bits(&br, 4);
            unsigned sidx = 15u - idx;
            uint8_t v = stash[sidx];
            for (unsigned k = sidx; k > 0; k--)
                stash[k] = stash[k - 1];
            stash[0] = v;
            dec[out_pos++] = v;
        } else if (cmd == 2 && hs_get_bit(&br) == 1) {
            if (out_pos >= total)
                break;
            uint8_t v = (uint8_t)hs_get_bits(&br, 8);
            if (nstash < 16)
                nstash++;
            for (unsigned k = nstash - 1; k > 0; k--)
                stash[k] = stash[k - 1];
            stash[0] = v;
            dec[out_pos++] = v;
        } else if (cmd == 6) {
            if (hs_get_bit(&br) == 1) {
                if (out_pos >= total)
                    break;
                if (nstash < 16)
                    nstash++;
                for (unsigned k = nstash - 1; k > 0; k--)
                    stash[k] = stash[k - 1];
                stash[0] = 0;
                dec[out_pos++] = 0;
            } else {
                uint32_t ord = hs_get_bits(&br, 2);
                uint32_t n = hs_get_bits(&br, (int)ord);
                prefix = 128u * ((1u << ord) - 1u - n);
            }
        } else {
            int lcmd = (cmd == 2) ? 2 : cmd;
            if (lcmd < 0 || lcmd >= 6)
                break;
            unsigned len = prefix + hs_get_gamma(&br);
            prefix = 0;
            int diff = origin[lcmd];
            if (diff <= 0 || out_pos < (size_t)diff)
                break;
            for (unsigned k = 0; k < len && out_pos < total; k++) {
                dec[out_pos] = dec[out_pos - (size_t)diff];
                out_pos++;
            }
            if (lcmd == 2) {
                int t = origin[2];
                origin[2] = origin[0];
                origin[0] = t;
            } else if (lcmd == 5) {
                int t = origin[5];
                origin[5] = origin[3];
                origin[3] = t;
            }
        }
    }

    CHECK(out_pos == total && memcmp(dec, band, total) == 0, "%s: Hi-SCoA round-trip mismatch (got %zu/%zu)", name,
          out_pos, total);
    free(comp);
    free(dec);
}

static void hiscoa_roundtrip_check(const uint8_t *band, unsigned ls, unsigned nlines, const char *name)
{
    hiscoa_roundtrip_check_mode(band, ls, nlines, 1, name);
    hiscoa_roundtrip_check_mode(band, ls, nlines, 0, name);
}

// ===========================================================================
// Brother Mode 1030 line decoder for round-trip testing
static int br_read_overflow(const uint8_t *in, size_t *p, int base)
{
    if (base < 0)
        return base;
    int v = base;
    for (;;) {
        uint8_t b = in[(*p)++];
        v += b;
        if (b != 255)
            break;
    }
    return v;
}

static void brother_roundtrip_check(const uint8_t *line, const uint8_t *ref, int lb, const char *name)
{
    uint8_t *enc = malloc((size_t)lb + 64u);
    uint8_t *dec = malloc((size_t)lb);
    size_t elen = brother_encode_line(line, ref, lb, enc);
    if (ref)
        memcpy(dec, ref, (size_t)lb);
    else
        memset(dec, 0, (size_t)lb);

    if (enc[0] == 0xffu) {
        CHECK(elen == 1, "%s: expected 1-byte zero marker", name);
        memset(dec, 0, (size_t)lb);
    } else {
        int edits = enc[0];
        size_t p = 1;
        int col = 0;
        for (int e = 0; e < edits; e++) {
            uint8_t cmd = enc[p++];
            if (cmd & 0x80u) {
                int off = (cmd >> 5) & 3;
                int cnt = cmd & 31;
                if (off == 3)
                    off = br_read_overflow(enc, &p, 3);
                if (cnt == 31)
                    cnt = br_read_overflow(enc, &p, 31);
                cnt += 2;
                col += off;
                uint8_t val = enc[p++];
                memset(dec + col, val, (size_t)cnt);
                col += cnt;
            } else {
                int off = (cmd >> 3) & 15;
                int cnt = cmd & 7;
                if (off == 15)
                    off = br_read_overflow(enc, &p, 15);
                if (cnt == 7)
                    cnt = br_read_overflow(enc, &p, 7);
                cnt += 1;
                col += off;
                memcpy(dec + col, enc + p, (size_t)cnt);
                p += (size_t)cnt;
                col += cnt;
            }
        }
        CHECK(p == elen, "%s: encoded byte length mismatch", name);
    }

    CHECK(memcmp(dec, line, (size_t)lb) == 0, "%s: Mode 1030 round-trip mismatch", name);
    free(enc);
    free(dec);
}

// ===========================================================================
// Mock bidirectional CAPT printer responder
typedef struct {
    mem_buf_t tx;
    uint8_t reply[96];
    size_t rlen;
    size_t rpos;
    bool bcd_a2a0;
    uint8_t fifo_free;
    uint16_t pages_started, pages_rx, pages_fired;
    bool sim_fifo;
    bool fired_before_c0a4;
    int cmds_seen[8]; // 0:A1A1, 1:A2A0, 2:D0A9, 3:C0A0, 4:C0A4, 5:E0A7, 6:E0A9
} mock_capt_t;

static int mock_capt_write(void *ctx, const uint8_t *data, size_t len)
{
    mock_capt_t *m = ctx;
    if (mem_write(&m->tx, data, len) != 0)
        return -1;
    // Parse complete CAPT packets at tail of tx as they arrive
    if (len >= 4) {
        uint16_t cmd = (uint16_t)(data[0] | (data[1] << 8));
        if (cmd == 0xA1A1) {
            m->cmds_seen[0]++;
            uint8_t r[6] = {0xA1, 0xA1, 0x06, 0x00, 0x00, 0x00};
            memcpy(m->reply, r, 6); m->rlen = 6; m->rpos = 0;
        } else if (cmd == 0xA2A0) {
            m->cmds_seen[1]++;
            if (m->bcd_a2a0) {
                // Simulate Canon CAPT BCD length quirk: 0x16 -> 16 decimal bytes (not 22)
                memset(m->reply, 0, 16);
                m->reply[0] = 0xA0; m->reply[1] = 0xA2; m->reply[2] = 0x16; m->reply[3] = 0x00;
                m->reply[6] = 0x2A; m->reply[7] = 0x00;
                m->rlen = 16; m->rpos = 0;
            } else {
                uint8_t r[8] = {0xA0, 0xA2, 0x08, 0x00, 0x00, 0x00, 0x2A, 0x00};
                memcpy(m->reply, r, 8); m->rlen = 8; m->rpos = 0;
            }
        } else if (cmd == 0xE0A0) {
            // Return STATUS0 = 0x0100 (CAPT_FL_XSTATUS_CHNG) so capt_poll_status reads 0xA0A8
            uint8_t r[6] = {0xA0, 0xE0, 0x06, 0x00, 0x00, 0x01};
            memcpy(m->reply, r, 6); m->rlen = 6; m->rpos = 0;
        } else if (cmd == 0xE1A9) {
            uint8_t r[6] = {0xA9, 0xE1, 0x06, 0x00, 0x00, 0x01};
            memcpy(m->reply, r, 6); m->rlen = 6; m->rpos = 0;
        } else if (cmd == 0xE0A1 || cmd == 0xE0A2 || cmd == 0xE0A3 ||
                   cmd == 0xE0A4 || cmd == 0xE0A5 || cmd == 0xE0A6 || cmd == 0xE0BA ||
                   cmd == 0xA3A2 || cmd == 0xE1A1 || cmd == 0xE1A2 || cmd == 0xA0A1) {
            uint8_t r[6] = {data[0], data[1], 0x06, 0x00, 0x00, 0x00};
            memcpy(m->reply, r, 6); m->rlen = 6; m->rpos = 0;
        } else if (cmd == 0xE0A7) {
            m->cmds_seen[5]++;
            if (m->pages_rx < m->pages_started)
                m->fired_before_c0a4 = true;
            m->pages_fired++;
            if (m->sim_fifo)
                m->fifo_free = 15;
            uint8_t r[6] = {0xA7, 0xE0, 0x06, 0x00, 0x00, 0x00};
            memcpy(m->reply, r, 6); m->rlen = 6; m->rpos = 0;
        } else if (cmd == 0xE0A9) {
            m->cmds_seen[6]++;
            uint8_t r[6] = {0xA9, 0xE0, 0x06, 0x00, 0x00, 0x00};
            memcpy(m->reply, r, 6); m->rlen = 6; m->rpos = 0;
        } else if (cmd == 0xA0A8) {
            memset(m->reply, 0, 88);
            m->reply[0] = 0xA8; m->reply[1] = 0xA0; m->reply[2] = 88; m->reply[3] = 0;
            uint16_t pdec = m->pages_started ? m->pages_started : 1;
            if (m->sim_fifo && m->pages_fired >= m->pages_started && m->fifo_free == 0)
                m->fifo_free = 15;
            m->reply[4 + 4] = m->sim_fifo ? m->fifo_free : 0x0F;
            m->reply[4 + 14] = (uint8_t)(pdec & 0xff);
            m->reply[4 + 15] = (uint8_t)(pdec >> 8);
            m->reply[4 + 18] = (uint8_t)(m->pages_fired & 0xff);
            m->reply[4 + 19] = (uint8_t)(m->pages_fired >> 8);
            m->reply[4 + 20] = (uint8_t)(m->pages_fired & 0xff);
            m->reply[4 + 21] = (uint8_t)(m->pages_fired >> 8);
            m->reply[4 + 34] = (uint8_t)(m->pages_rx & 0xff);
            m->reply[4 + 35] = (uint8_t)(m->pages_rx >> 8);
            m->rlen = 88; m->rpos = 0;
        } else if (cmd == 0xD0A9) {
            m->cmds_seen[2]++;
            m->pages_started++;
        } else if (cmd == 0xC0A0) {
            m->cmds_seen[3]++;
            if (m->sim_fifo && m->fifo_free > 0)
                m->fifo_free--;
        } else if (cmd == 0xC0A4) {
            m->cmds_seen[4]++;
            m->pages_rx++;
        }
    }
    return 0;
}

static int mock_capt_flush(void *ctx)
{
    (void)ctx;
    return 0;
}

static int mock_capt_read(void *ctx, uint8_t *buf, size_t len, int timeout_ms)
{
    (void)timeout_ms;
    mock_capt_t *m = ctx;
    if (m->rpos >= m->rlen)
        return 0;
    size_t avail = m->rlen - m->rpos;
    size_t take = avail < len ? avail : len;
    memcpy(buf, m->reply + m->rpos, take);
    m->rpos += take;
    return (int)take;
}

int main(void)
{
    // 1. Check zero duplicate (vid, pid) across all drivers
    int drv_count = 0;
    for (int i = 0; printer_drivers[i]; i++) {
        drv_count++;
        for (int j = i + 1; printer_drivers[j]; j++) {
            const printer_driver_t *a = printer_drivers[i], *b = printer_drivers[j];
            if (a->usb_vid && a->usb_pid && b->usb_vid && b->usb_pid) {
                CHECK(!(a->usb_vid == b->usb_vid && a->usb_pid == b->usb_pid),
                      "duplicate USB VID:PID %04x:%04x (%s vs %s)", a->usb_vid, a->usb_pid, a->id, b->id);
            }
        }
    }
    CHECK(drv_count >= 62, "expected >= 62 drivers, got %d", drv_count);

    // 2. JBIG1 round-trip tests
    {
        uint32_t w = 259, h = 270, lb = (w + 7) / 8;
        uint8_t *img = calloc(h, lb);
        jbig1_roundtrip_check(w, h, 128, img, "jbig-white");
        memset(img, 0xff, (size_t)h * lb);
        for (uint32_t y = 0; y < h; y++)
            img[y * lb + lb - 1] &= (uint8_t)(0xffu << (8 - (w & 7)));
        jbig1_roundtrip_check(w, h, 128, img, "jbig-black");
        memset(img, 0, (size_t)h * lb);
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++)
                if ((x ^ y) & 1)
                    img[y * lb + (x >> 3)] |= (uint8_t)(0x80u >> (x & 7));
        // Clear unused trailing bits on each line
        for (uint32_t y = 0; y < h; y++)
            img[y * lb + lb - 1] &= (uint8_t)(0xffu << (8 - (w & 7)));
        jbig1_roundtrip_check(w, h, 128, img, "jbig-checker");
        srand(42);
        for (uint32_t y = 0; y < h; y++) {
            for (uint32_t bi = 0; bi < lb; bi++)
                img[y * lb + bi] = (uint8_t)rand();
            img[y * lb + lb - 1] &= (uint8_t)(0xffu << (8 - (w & 7)));
        }
        jbig1_roundtrip_check(w, h, 128, img, "jbig-random");
        free(img);
    }

    // 3. Hi-SCoA round-trip tests
    {
        unsigned ls = 64, nl = 70;
        uint8_t *band = calloc(nl, ls);
        hiscoa_roundtrip_check(band, ls, nl, "hiscoa-zero");
        for (unsigned y = 0; y < nl; y++)
            for (unsigned x = 0; x < ls; x++)
                band[y * ls + x] = (uint8_t)((y / 4) * 17 + (x < 10 ? 0 : x & 3));
        hiscoa_roundtrip_check(band, ls, nl, "hiscoa-pattern");
        srand(99);
        for (unsigned i = 0; i < nl * ls; i++)
            band[i] = (uint8_t)(rand() % 5 == 0 ? rand() : (i > 0 ? band[i - 1] : 0));
        hiscoa_roundtrip_check(band, ls, nl, "hiscoa-mixed");
        free(band);
    }

    // 4. Brother Mode 1030 round-trip tests
    {
        int lb = 592;
        uint8_t *line = calloc(1, (size_t)lb);
        uint8_t *ref = calloc(1, (size_t)lb);
        brother_roundtrip_check(line, ref, lb, "br-zero");
        memset(line, 0xaa, (size_t)lb);
        brother_roundtrip_check(line, NULL, lb, "br-noref-repeat");
        memcpy(ref, line, (size_t)lb);
        line[10] = 0x55; line[11] = 0x66; line[400] = 0x12;
        brother_roundtrip_check(line, ref, lb, "br-delta");
        srand(7);
        for (int i = 0; i < lb; i++)
            line[i] = (uint8_t)rand();
        brother_roundtrip_check(line, ref, lb, "br-random");
        free(line);
        free(ref);
    }

    // 5. HP LaserJet ZjStream & XQX stream framing tests
    {
        const char *hp_ids[] = {"hp-lj1020", "hp-p1102", "hp-p1005", "hp-m1132"};
        for (size_t i = 0; i < sizeof(hp_ids) / sizeof(hp_ids[0]); i++) {
            const printer_driver_t *d = printer_driver_find(hp_ids[i]);
            CHECK(d != NULL && !d->color && !d->scanner, "find %s", hp_ids[i]);
            mem_buf_t out = {0};
            printer_io_t io = {.write = mem_write, .ctx = &out};
            void *job = d->job_begin(d, &io, 600, 0);
            CHECK(job != NULL, "%s job_begin", hp_ids[i]);
            CHECK(d->page_begin(job, 64, 32, 3) == 0, "%s page_begin", hp_ids[i]);
            uint8_t px[64 * 3];
            memset(px, 0x80, sizeof(px));
            for (int y = 0; y < 32; y++)
                CHECK(d->page_line(job, y, px) == 0, "%s page_line", hp_ids[i]);
            CHECK(d->page_end(job) == 0, "%s page_end", hp_ids[i]);
            CHECK(d->job_end(job) == 0, "%s job_end", hp_ids[i]);
            if (!strcmp(hp_ids[i], "hp-lj1020"))
                CHECK(out.len > 40 && !memcmp(out.buf, "JZJZ", 4), "lj1020 JZJZ magic");
            else if (!strcmp(hp_ids[i], "hp-p1102"))
                CHECK(out.len > 100 && strstr((const char *)out.buf, "@PJL ENTER LANGUAGE=ACL"), "p1102 PJL+ACL");
            else
                CHECK(out.len > 100 && strstr((const char *)out.buf, "@PJL ENTER LANGUAGE=XQX"), "%s PJL+XQX", hp_ids[i]);
            free(out.buf);
        }
    }

    // 6. Canon LBP CAPT bidirectional mock + offline test
    {
        const printer_driver_t *d = printer_driver_find("canon-lbp2900");
        CHECK(d != NULL && !d->color, "canon-lbp2900");
        mock_capt_t mc = {0};
        printer_io_t io = {.write = mock_capt_write, .flush = mock_capt_flush, .read = mock_capt_read, .ctx = &mc};
        void *job = d->job_begin(d, &io, 600, 0);
        CHECK(job != NULL, "lbp2900 bidir job_begin");
        CHECK(d->page_begin(job, 64, 16, 1) == 0, "lbp2900 page_begin");
        uint8_t px[64];
        memset(px, 0x40, sizeof(px));
        for (int y = 0; y < 16; y++)
            CHECK(d->page_line(job, y, px) == 0, "lbp2900 page_line");
        CHECK(d->page_end(job) == 0, "lbp2900 page_end");
        CHECK(d->job_end(job) == 0, "lbp2900 job_end");
        for (int c = 0; c < 7; c++)
            CHECK(mc.cmds_seen[c] > 0, "lbp2900 command slot %d not seen", c);
        free(mc.tx.buf);
    }

    // 7. Brother Laser PJL/PCL Mode 1030 framing test
    {
        const printer_driver_t *d = printer_driver_find("brother-hl1110");
        CHECK(d != NULL && !d->color && !d->scanner, "brother-hl1110");
        mem_buf_t out = {0};
        printer_io_t io = {.write = mem_write, .ctx = &out};
        void *job = d->job_begin(d, &io, 600, 0);
        CHECK(job != NULL, "brother job_begin");
        CHECK(d->page_begin(job, 64, 16, 1) == 0, "brother page_begin");
        uint8_t px[64];
        memset(px, 0, sizeof(px));
        for (int y = 0; y < 16; y++)
            CHECK(d->page_line(job, y, px) == 0, "brother page_line");
        CHECK(d->page_end(job) == 0, "brother page_end");
        CHECK(d->job_end(job) == 0, "brother job_end");
        CHECK(out.len > 200 && memcmp(out.buf + 128, "\x1b%-12345X@PJL", 13) == 0, "brother PJL header");
        free(out.buf);
    }

    // 8. Canon LBP3300 & duplex driver tests
    {
        const printer_driver_t *d = printer_driver_find("canon-lbp3300");
        CHECK(d != NULL && d->usb_vid == 0x04a9 && d->usb_pid == 0x267e && d->duplex, "canon-lbp3300 caps");
        char urf[96];
        printer_driver_urf(d, urf, sizeof(urf));
        CHECK(strstr(urf, "DM3") != NULL, "canon-lbp3300 urf missing DM3: %s", urf);

        // 2-page duplex CAPT bidirectional mock test (with BCD length quirk on 0xA2A0)
        mock_capt_t mc = {.bcd_a2a0 = true};
        printer_io_t io = {.write = mock_capt_write,
                           .flush = mock_capt_flush,
                           .read = mock_capt_read,
                           .sides = PRINTER_SIDES_TWO_SIDED_LONG_EDGE,
                           .ctx = &mc};
        void *job = d->job_begin(d, &io, 600, 0);
        CHECK(job != NULL, "lbp3300 bidir job_begin");
        uint8_t px[64];
        memset(px, 0x40, sizeof(px));
        for (int pg = 0; pg < 2; pg++) {
            CHECK(d->page_begin(job, 64, 16, 1) == 0, "lbp3300 page %d begin", pg + 1);
            for (int y = 0; y < 16; y++)
                CHECK(d->page_line(job, y, px) == 0, "lbp3300 page %d line", pg + 1);
            CHECK(d->page_end(job) == 0, "lbp3300 page %d end", pg + 1);
        }
        CHECK(d->job_end(job) == 0, "lbp3300 job_end");
        for (int c = 0; c < 7; c++)
            CHECK(mc.cmds_seen[c] > 0, "lbp3300 command slot %d not seen", c);

        // Locate both 0xD0A0 page parameter blocks (A0 D0 2C 00 ...)
        int found_pages = 0;
        for (size_t k = 0; k + 44 <= mc.tx.len; k++) {
            if (mc.tx.buf[k] == 0xA0 && mc.tx.buf[k + 1] == 0xD0 && mc.tx.buf[k + 2] == 0x2C && mc.tx.buf[k + 3] == 0x00) {
                const uint8_t *pp = mc.tx.buf + k + 4;
                found_pages++;
                CHECK(pp[5] == (found_pages == 1 ? 0x04 : 0x02), "lbp3300 page %d pp[5] duplex side", found_pages);
                CHECK(pp[6] == 0x01, "lbp3300 page %d pp[6] cassette 1", found_pages);
                CHECK(pp[20] == 0x00, "lbp3300 page %d pp[20] flag", found_pages);
            }
        }
        CHECK(found_pages == 2, "lbp3300 expected 2 D0A0 blocks, found %d", found_pages);
        CHECK(mc.pages_started == 2 && mc.pages_fired == 2, "lbp3300 2-page counters %u/%u", mc.pages_started,
              mc.pages_fired);
        free(mc.tx.buf);
    }
    {
        // Canon LBP3300 FIFO-full FlyingStart test: 5 bands (> 3 C0A0 packets)
        const printer_driver_t *d = printer_driver_find("canon-lbp3300");
        CHECK(d != NULL, "canon-lbp3300 fifo");
        mock_capt_t mc = {.sim_fifo = true, .fifo_free = 3};
        printer_io_t io = {.write = mock_capt_write, .flush = mock_capt_flush, .read = mock_capt_read, .ctx = &mc};
        void *job = d->job_begin(d, &io, 600, 0);
        CHECK(job != NULL, "lbp3300 fifo job_begin");
        CHECK(d->page_begin(job, 64, 300, 1) == 0, "lbp3300 fifo page_begin");
        uint8_t px[64];
        memset(px, 0x40, sizeof(px));
        for (int y = 0; y < 300; y++)
            CHECK(d->page_line(job, y, px) == 0, "lbp3300 fifo page_line");
        CHECK(d->page_end(job) == 0, "lbp3300 fifo page_end");
        CHECK(d->job_end(job) == 0, "lbp3300 fifo job_end");
        CHECK(mc.cmds_seen[3] >= 4, "lbp3300 fifo expected >= 4 C0A0 packets, got %d", mc.cmds_seen[3]);
        CHECK(mc.fired_before_c0a4, "lbp3300 fifo did not trigger mid-page FlyingStart");
        CHECK(mc.cmds_seen[5] == 1, "lbp3300 fifo expected 1 E0A7 fire, got %d", mc.cmds_seen[5]);
        free(mc.tx.buf);
    }
    {
        const printer_driver_t *d = printer_driver_find("hp-p1606dn");
        CHECK(d != NULL && d->duplex, "hp-p1606dn duplex");
        mem_buf_t out = {0};
        printer_io_t io = {.write = mem_write, .sides = PRINTER_SIDES_TWO_SIDED_LONG_EDGE, .ctx = &out};
        void *job = d->job_begin(d, &io, 600, 0);
        CHECK(job != NULL, "hp-p1606dn job_begin");
        CHECK(d->page_begin(job, 64, 16, 1) == 0, "hp-p1606dn page_begin");
        uint8_t px[64];
        memset(px, 0, sizeof(px));
        for (int y = 0; y < 16; y++)
            CHECK(d->page_line(job, y, px) == 0, "hp-p1606dn page_line");
        CHECK(d->page_end(job) == 0, "hp-p1606dn page_end");
        CHECK(d->job_end(job) == 0, "hp-p1606dn job_end");
        static const uint8_t zji_dup[12] = {0, 0, 0, 12, 0, 2, 1, 0, 0, 0, 0, 2};
        bool found_dup = false;
        for (size_t k = 0; k + sizeof(zji_dup) <= out.len; k++) {
            if (!memcmp(out.buf + k, zji_dup, sizeof(zji_dup))) {
                found_dup = true;
                break;
            }
        }
        CHECK(found_dup, "hp-p1606dn ZJI_DMDUPLEX=2 missing");
        free(out.buf);
    }
    {
        const printer_driver_t *d = printer_driver_find("brother-hll2300d");
        CHECK(d != NULL && d->duplex, "brother-hll2300d duplex");
        mem_buf_t out = {0};
        printer_io_t io = {.write = mem_write, .sides = PRINTER_SIDES_TWO_SIDED_LONG_EDGE, .ctx = &out};
        void *job = d->job_begin(d, &io, 600, 0);
        CHECK(job != NULL, "brother-hll2300d job_begin");
        CHECK(d->page_begin(job, 64, 16, 1) == 0, "brother-hll2300d page_begin");
        uint8_t px[64];
        memset(px, 0, sizeof(px));
        for (int y = 0; y < 16; y++)
            CHECK(d->page_line(job, y, px) == 0, "brother-hll2300d page_line");
        CHECK(d->page_end(job) == 0, "brother-hll2300d page_end");
        CHECK(d->job_end(job) == 0, "brother-hll2300d job_end");
        uint8_t z = 0;
        mem_write(&out, &z, 1);
        CHECK(out.len > 128 &&
                  strstr((const char *)out.buf + 128, "@PJL SET DUPLEX = ON") != NULL &&
                  strstr((const char *)out.buf + 128, "@PJL SET BINDING = LONGEDGE") != NULL,
              "brother-hll2300d PJL duplex headers missing");
        free(out.buf);
    }

    printf("drivers_test: %s (%d failures, %d drivers registered)\n", failures ? "FAILED" : "OK", failures, drv_count);
    return failures != 0;
}
