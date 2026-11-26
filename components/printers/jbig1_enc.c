#include "jbig1_enc.h"

#include <stdlib.h>
#include <string.h>

// ITU-T T.82 Table 24 probability estimation table (113 states).
static const uint16_t QM_LSZ[113] = {
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

static const uint8_t QM_NMPS[113] = {
      1,   2,   3,   4,   5,   6,   7,   8,
      9,  10,  11,  12,  13,  13,  15,  16,
     17,  18,  19,  20,  21,  22,  23,  24,
     25,  26,  27,  28,  29,  30,  31,  32,
     33,  34,  35,   9,  37,  38,  39,  40,
     41,  42,  43,  44,  45,  46,  47,  48,
     49,  50,  51,  52,  53,  54,  55,  56,
     57,  58,  59,  60,  61,  62,  63,  32,
     65,  66,  67,  68,  69,  70,  71,  72,
     73,  74,  75,  76,  77,  78,  79,  48,
     81,  82,  83,  84,  85,  86,  87,  71,
     89,  90,  91,  92,  93,  94,  86,  96,
     97,  98,  99, 100,  93, 102, 103, 104,
     99, 106, 107, 103, 109, 107, 111, 109,
    111,
};

// Bits 6..0 = NLPS, bit 7 = SWTCH
static const uint8_t QM_NLPS[113] = {
    129,  14,  16,  18,  20,  23,  25,  28,
     30,  33,  35,   9,  10,  12, 143,  36,
     38,  39,  40,  42,  43,  45,  46,  48,
     49,  51,  52,  54,  56,  57,  59,  60,
     62,  63,  32,  33, 165,  64,  65,  67,
     68,  69,  70,  72,  73,  74,  75,  77,
     78,  79,  48,  50,  50,  51,  52,  53,
     54,  55,  56,  57,  58,  59,  61,  61,
    193,  80,  81,  82,  83,  84,  86,  87,
     87,  72,  72,  74,  74,  75,  77,  77,
    208,  88,  89,  90,  91,  92,  93,  86,
    216,  95,  96,  97,  99,  99,  93, 223,
    101, 102, 103, 104,  99, 105, 106, 107,
    103, 233, 108, 109, 110, 111, 238, 112,
    240,
};

static void emit_flush_buf(jbig1_enc_t *e)
{
    if (e->out_len > 0 && !e->err) {
        if (e->sink(e->user, e->out, e->out_len) != 0)
            e->err = -1;
        e->out_len = 0;
    }
}

static void emit_byte(jbig1_enc_t *e, uint8_t b)
{
    if (e->err)
        return;
    e->out[e->out_len++] = b;
    if (e->out_len == sizeof(e->out))
        emit_flush_buf(e);
}

static void qm_init(jbig1_enc_t *e, int reuse_ctx)
{
    if (!reuse_ctx)
        memset(e->ctx, 0, sizeof(e->ctx));
    e->a = 0x10000u;
    e->c = 0;
    e->ct = 11;
    e->sc = 0;
    e->buffer = -1;
}

static void qm_byteout(jbig1_enc_t *e)
{
    uint32_t temp = e->c >> 19;
    if (temp > 0xffu) {
        if (e->buffer >= 0) {
            uint8_t b = (uint8_t)(e->buffer + 1);
            emit_byte(e, b);
            if (b == 0xffu)
                emit_byte(e, 0x00u);
        }
        for (; e->sc > 0; e->sc--)
            emit_byte(e, 0x00u);
        e->buffer = (int)(temp & 0xffu);
    } else if (temp == 0xffu) {
        e->sc++;
    } else {
        if (e->buffer >= 0)
            emit_byte(e, (uint8_t)e->buffer);
        for (; e->sc > 0; e->sc--) {
            emit_byte(e, 0xffu);
            emit_byte(e, 0x00u);
        }
        e->buffer = (int)temp;
    }
    e->c &= 0x7ffffu;
    e->ct = 8;
}

static void qm_encode(jbig1_enc_t *e, uint32_t cx, int pix)
{
    uint8_t *st = &e->ctx[cx];
    uint8_t ss = *st & 0x7fu;
    uint32_t lsz = QM_LSZ[ss];

    if (((uint32_t)(pix << 7) ^ *st) & 0x80u) {
        // LPS
        e->a -= lsz;
        if (e->a >= lsz) {
            e->c += e->a;
            e->a = lsz;
        }
        *st = (uint8_t)((*st & 0x80u) ^ QM_NLPS[ss]);
    } else {
        // MPS
        e->a -= lsz;
        if (e->a >= 0x8000u)
            return;
        if (e->a < lsz) {
            e->c += e->a;
            e->a = lsz;
        }
        *st = (uint8_t)((*st & 0x80u) | QM_NMPS[ss]);
    }

    do {
        e->a <<= 1;
        e->c <<= 1;
        if (--e->ct == 0)
            qm_byteout(e);
    } while (e->a < 0x8000u);
}

static void qm_flush(jbig1_enc_t *e)
{
    uint32_t temp = (e->a - 1u + e->c) & 0xffff0000u;
    if (temp < e->c)
        e->c = temp + 0x8000u;
    else
        e->c = temp;

    e->c <<= e->ct;
    if (e->c & 0xf8000000u) {
        if (e->buffer >= 0) {
            uint8_t b = (uint8_t)(e->buffer + 1);
            emit_byte(e, b);
            if (b == 0xffu)
                emit_byte(e, 0x00u);
        }
        if (e->c & 0x07fff800u) {
            for (; e->sc > 0; e->sc--)
                emit_byte(e, 0x00u);
        } else {
            e->sc = 0;
        }
    } else {
        if (e->buffer >= 0)
            emit_byte(e, (uint8_t)e->buffer);
        for (; e->sc > 0; e->sc--) {
            emit_byte(e, 0xffu);
            emit_byte(e, 0x00u);
        }
    }

    if (e->c & 0x07fff800u) {
        uint8_t b1 = (uint8_t)((e->c >> 19) & 0xffu);
        emit_byte(e, b1);
        if (b1 == 0xffu)
            emit_byte(e, 0x00u);
        if (e->c & 0x0007f800u) {
            uint8_t b2 = (uint8_t)((e->c >> 11) & 0xffu);
            emit_byte(e, b2);
            if (b2 == 0xffu)
                emit_byte(e, 0x00u);
        }
    }
}

int jbig1_enc_init(jbig1_enc_t *enc, uint32_t width, uint32_t height, uint32_t l0, jbig1_sink_fn sink, void *user)
{
    if (!enc || !sink || width == 0 || height == 0 || l0 == 0)
        return -1;
    memset(enc, 0, sizeof(*enc));
    enc->width = width;
    enc->height = height;
    enc->l0 = l0;
    enc->line_bytes = (width + 7u) / 8u;
    enc->sink = sink;
    enc->user = user;
    enc->prev_line = calloc(1, enc->line_bytes);
    if (!enc->prev_line)
        return -1;
    qm_init(enc, 0);
    return 0;
}

void jbig1_enc_get_bih(const jbig1_enc_t *enc, uint8_t bih[20])
{
    memset(bih, 0, 20);
    bih[0] = 0;     // DL = 0
    bih[1] = 0;     // D = 0
    bih[2] = 1;     // P = 1
    bih[3] = 0;
    bih[4] = (uint8_t)(enc->width >> 24);
    bih[5] = (uint8_t)(enc->width >> 16);
    bih[6] = (uint8_t)(enc->width >> 8);
    bih[7] = (uint8_t)(enc->width);
    bih[8] = (uint8_t)(enc->height >> 24);
    bih[9] = (uint8_t)(enc->height >> 16);
    bih[10] = (uint8_t)(enc->height >> 8);
    bih[11] = (uint8_t)(enc->height);
    bih[12] = (uint8_t)(enc->l0 >> 24);
    bih[13] = (uint8_t)(enc->l0 >> 16);
    bih[14] = (uint8_t)(enc->l0 >> 8);
    bih[15] = (uint8_t)(enc->l0);
    bih[16] = 0;    // MX = 0
    bih[17] = 0;    // MY = 0
    bih[18] = 0;    // ORDER = 0
    bih[19] = JBIG1_OPT_LRLTWO;
}

int jbig1_enc_add_line(jbig1_enc_t *enc, const uint8_t *bits)
{
    if (!enc || enc->err)
        return -1;
    if (enc->y >= enc->height)
        return 0;

    uint32_t lb = enc->line_bytes;
    uint32_t h1 = 0;
    uint32_t h2 = (uint32_t)enc->prev_line[0] << 8;

    for (uint32_t byte_i = 0; byte_i < lb && !enc->err; byte_i++) {
        uint8_t cur_b = bits ? bits[byte_i] : 0;
        uint8_t nxt_prev = (byte_i + 1u < lb) ? enc->prev_line[byte_i + 1u] : 0;
        h1 |= cur_b;
        h2 |= nxt_prev;

        uint32_t px_base = byte_i * 8u;
        uint32_t count = enc->width - px_base;
        if (count > 8u)
            count = 8u;

        for (uint32_t b = 0; b < count; b++) {
            h1 <<= 1;
            h2 <<= 1;
            uint32_t cx = ((h2 >> 10) & 0x3f0u) | ((h1 >> 9) & 0x00fu);
            int pix = (int)((h1 >> 8) & 1u);
            qm_encode(enc, cx, pix);
        }
    }

    if (bits)
        memcpy(enc->prev_line, bits, lb);
    else
        memset(enc->prev_line, 0, lb);

    enc->y++;
    if ((enc->y % enc->l0) == 0 || enc->y == enc->height) {
        qm_flush(enc);
        emit_byte(enc, 0xffu);
        emit_byte(enc, 0x02u);  // SDNORM
        emit_flush_buf(enc);
        if (enc->y < enc->height)
            qm_init(enc, 1);    // reuse contexts across stripes
    }
    return enc->err ? -1 : 0;
}

int jbig1_enc_finish(jbig1_enc_t *enc)
{
    if (!enc)
        return -1;
    while (!enc->err && enc->y < enc->height) {
        if (jbig1_enc_add_line(enc, NULL) != 0)
            break;
    }
    emit_flush_buf(enc);
    return enc->err ? -1 : 0;
}

void jbig1_enc_free(jbig1_enc_t *enc)
{
    if (!enc)
        return;
    free(enc->prev_line);
    enc->prev_line = NULL;
}
