// Portable clean-room ITU-T T.82 (JBIG1) sequential single-layer encoder.
// Produces a 20-byte BIH + SDNORM-delimited 2-line-template (LRLTWO) stream
// compatible with HP ZjStream and XQX laser printers.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JBIG1_OPT_LRLTWO 0x40u

typedef int (*jbig1_sink_fn)(void *user, const uint8_t *data, size_t len);

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t l0;            // lines per stripe (e.g. 128)
    uint32_t line_bytes;    // (width + 7) / 8
    uint32_t y;             // lines encoded so far
    jbig1_sink_fn sink;
    void *user;
    int err;

    // 2-line template previous scanline + scratch current scanline
    uint8_t *prev_line;
    // 1024-entry QM-coder context state (bit 7 = MPS, bits 6..0 = state 0..112)
    uint8_t ctx[1024];

    // QM-coder registers (ITU-T T.82)
    uint32_t a;
    uint32_t c;
    int ct;
    int sc;
    int buffer;

    // Small output staging buffer to batch sink writes
    uint8_t out[512];
    size_t out_len;
} jbig1_enc_t;

// Initializes encoder for width x height 1-bit MSB-first image (1 = black).
// Returns 0 on success, -1 on allocation/argument error.
int jbig1_enc_init(jbig1_enc_t *enc, uint32_t width, uint32_t height, uint32_t l0, jbig1_sink_fn sink, void *user);

// Writes the 20-byte big-endian JBIG1 BIH header into bih[20].
void jbig1_enc_get_bih(const jbig1_enc_t *enc, uint8_t bih[20]);

// Encodes one 1-bit MSB-first scanline (line_bytes bytes). bits == NULL encodes all-white.
int jbig1_enc_add_line(jbig1_enc_t *enc, const uint8_t *bits);

// Pads remaining lines (if y < height) with white, flushes final stripe + SDNORM.
int jbig1_enc_finish(jbig1_enc_t *enc);

void jbig1_enc_free(jbig1_enc_t *enc);

#ifdef __cplusplus
}
#endif
