// Brother monochrome laser printer driver (PJL + PCL + brlaser Mode 1030
// band delta/RLE raster compression). Clean-room implementation.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mono_raster.h"
#include "printer_driver.h"

#define BRL_LINES_PER_BAND 64
#define BRL_MAX_BLOCK_BYTES 16350

typedef struct {
    const printer_driver_t *drv;
    printer_io_t io;
    int dpi;
    int paper;
    int width;
    int height;
    int line_bytes;
    int src_w;
    int src_h;
    int bytes_pp;

    uint8_t *density;
    uint8_t *cur;
    uint8_t *ref;
    uint8_t *line_enc;      // encoded single line buffer
    uint8_t *block;         // accumulated lines in current band
    size_t block_len;
    int block_lines;
    int pages;
    int y;
    bool in_page;
    bool err;
} br_job_t;

static int br_write_all(br_job_t *j, const void *data, size_t len)
{
    if (j->err)
        return -1;
    if (j->io.write(j->io.ctx, (const uint8_t *)data, len) < 0) {
        j->err = true;
        return -1;
    }
    return 0;
}

static int br_write_str(br_job_t *j, const char *s)
{
    return br_write_all(j, s, strlen(s));
}

static void write_overflow(int val, uint8_t *out, size_t *pos)
{
    if (val < 0)
        return;
    while (val >= 255) {
        out[(*pos)++] = 255;
        val -= 255;
    }
    out[(*pos)++] = (uint8_t)val;
}

static void write_substitute(int offset, const uint8_t *data, int count_bytes, uint8_t *out, size_t *pos)
{
    int cnt = count_bytes - 1;
    int off_lo = offset < 15 ? offset : 15;
    int cnt_lo = cnt < 7 ? cnt : 7;
    out[(*pos)++] = (uint8_t)((off_lo << 3) | cnt_lo);
    write_overflow(offset - 15, out, pos);
    write_overflow(cnt - 7, out, pos);
    memcpy(out + *pos, data, (size_t)count_bytes);
    *pos += (size_t)count_bytes;
}

static void write_repeat(int offset, int count, uint8_t val, uint8_t *out, size_t *pos)
{
    int cnt = count - 2;
    int off_lo = offset < 3 ? offset : 3;
    int cnt_lo = cnt < 31 ? cnt : 31;
    out[(*pos)++] = (uint8_t)(0x80u | (off_lo << 5) | cnt_lo);
    write_overflow(offset - 3, out, pos);
    write_overflow(cnt - 31, out, pos);
    out[(*pos)++] = val;
}

// Encodes one 1-bit scanline in Mode 1030 format (relative to reference if non-NULL).
size_t brother_encode_line(const uint8_t *line, const uint8_t *ref, int line_bytes, uint8_t *out)
{
    bool all_zero = true;
    for (int i = 0; i < line_bytes; i++) {
        if (line[i] != 0) {
            all_zero = false;
            break;
        }
    }
    if (all_zero) {
        out[0] = 0xffu;
        return 1;
    }
    if (!ref) {
        size_t pos = 0;
        out[pos++] = 1;
        write_substitute(0, line, line_bytes, out, &pos);
        return pos;
    }

    int end = line_bytes;
    while (end > 0 && line[end - 1] == ref[end - 1])
        end--;

    size_t pos = 1; // byte 0 is edit count
    int num_edits = 0;
    const int max_edits = 254;
    int i = 0;

    for (;;) {
        int off = 0;
        while (i < end && line[i] == ref[i]) {
            i++;
            off++;
        }
        if (i >= end)
            break;

        if (++num_edits == max_edits) {
            write_substitute(off, line + i, end - i, out, &pos);
            break;
        }

        // Check whether a literal substitute run is better before next match or 3-byte repeat
        int sub = 0;
        while (i + sub + 1 < end) {
            if (line[i + sub] == ref[i + sub] && line[i + sub + 1] == ref[i + sub + 1])
                break;
            if (sub > 0 && line[i + sub] == line[i + sub + 1] && line[i + sub] == line[i + sub - 1]) {
                sub--;
                break;
            }
            sub++;
        }
        if (i + sub + 1 >= end)
            sub = end - i;

        if (sub > 0) {
            write_substitute(off, line + i, sub, out, &pos);
            i += sub;
        } else {
            int rep = 1;
            uint8_t v = line[i];
            while (i + rep < end && line[i + rep] == v)
                rep++;
            write_repeat(off, rep, v, out, &pos);
            i += rep;
        }
    }

    out[0] = (uint8_t)num_edits;
    return pos;
}

static int flush_band(br_job_t *j)
{
    if (j->block_len == 0 || j->err)
        return j->err ? -1 : 0;
    char hdr[32];
    int n = snprintf(hdr, sizeof(hdr), "%zuw", j->block_len + 2);
    hdr[n++] = 0;
    hdr[n++] = (char)(uint8_t)j->block_lines;
    br_write_all(j, hdr, (size_t)n);
    br_write_all(j, j->block, j->block_len);
    j->block_len = 0;
    j->block_lines = 0;
    return j->err ? -1 : 0;
}

static void br_page_dims(const printer_driver_t *d, int dpi, int paper, int *w_out, int *h_out)
{
    const printer_paper_t *pp = &d->papers[paper];
    int pw = (int)((long)pp->width * dpi / 2540);
    int ph = (int)((long)pp->height * dpi / 2540);
    int ml = (int)((long)d->margin_left * dpi / 2540);
    int mr = (int)((long)d->margin_right * dpi / 2540);
    int mt = (int)((long)d->margin_top * dpi / 2540);
    int mb = (int)((long)d->margin_bottom * dpi / 2540);
    *w_out = ((pw - ml - mr) + 7) & ~7;
    *h_out = ph - mt - mb;
}

static void *job_begin(const printer_driver_t *d, const printer_io_t *io, int dpi, int paper)
{
    br_job_t *j = calloc(1, sizeof(*j));
    if (!j)
        return NULL;
    j->drv = d;
    j->io = *io;
    j->dpi = dpi;
    j->paper = paper;
    br_page_dims(d, dpi, paper, &j->width, &j->height);
    j->line_bytes = (j->width + 7) / 8;
    j->density = malloc((size_t)j->width);
    j->cur = malloc((size_t)j->line_bytes);
    j->ref = malloc((size_t)j->line_bytes);
    j->line_enc = malloc((size_t)j->line_bytes + 64u);
    j->block = malloc(BRL_MAX_BLOCK_BYTES);
    if (!j->density || !j->cur || !j->ref || !j->line_enc || !j->block) {
        free(j->density);
        free(j->cur);
        free(j->ref);
        free(j->line_enc);
        free(j->block);
        free(j);
        return NULL;
    }

    uint8_t zeros[128] = {0};
    br_write_all(j, zeros, sizeof(zeros));
    br_write_str(j, "\x1b%-12345X@PJL\n@PJL JOB NAME=\"AirPrint\"\n");
    return j;
}

static int page_begin(void *job, int src_w, int src_h, int bytes_pp)
{
    br_job_t *j = job;
    if (j->err)
        return -1;
    j->src_w = src_w;
    j->src_h = src_h;
    j->bytes_pp = bytes_pp;
    j->block_len = 0;
    j->block_lines = 0;
    j->y = 0;
    j->in_page = true;

    if (j->pages == 0) {
        int sides = j->drv->duplex ? j->io.sides : PRINTER_SIDES_ONE_SIDED;
        const char *duplex_pjl = sides == PRINTER_SIDES_TWO_SIDED_LONG_EDGE
                                     ? "@PJL SET DUPLEX = ON\n@PJL SET BINDING = LONGEDGE\n"
                                 : sides == PRINTER_SIDES_TWO_SIDED_SHORT_EDGE
                                     ? "@PJL SET DUPLEX = ON\n@PJL SET BINDING = SHORTEDGE\n"
                                     : "@PJL SET DUPLEX = OFF\n";
        char pjl[384];
        snprintf(pjl, sizeof(pjl),
                 "\x1b%%-12345X@PJL\n"
                 "@PJL SET RAS1200MODE = FALSE\n"
                 "@PJL SET RESOLUTION = %d\n"
                 "@PJL SET ECONOMODE = OFF\n"
                 "@PJL SET SOURCETRAY = AUTO\n"
                 "@PJL SET MEDIATYPE = REGULAR\n"
                 "@PJL SET PAPER = %s\n"
                 "@PJL SET PAGEPROTECT = AUTO\n"
                 "@PJL SET ORIENTATION = PORTRAIT\n"
                 "%s"
                 "@PJL ENTER LANGUAGE = PCL\n"
                 "\033E\033&l1X",
                 j->dpi, j->paper == 1 ? "LETTER" : "A4", duplex_pjl);
        br_write_str(j, pjl);
    }
    j->pages++;
    br_write_str(j, "\x1b*b1030m");
    return j->err ? -1 : 0;
}

static int emit_encoded_line(br_job_t *j, const uint8_t *bits)
{
    bool start_band = (j->y % BRL_LINES_PER_BAND) == 0;
    if (start_band && j->block_len > 0)
        flush_band(j);
    size_t elen = brother_encode_line(bits, start_band ? NULL : j->ref, j->line_bytes, j->line_enc);
    if (j->block_len + elen >= BRL_MAX_BLOCK_BYTES || j->block_lines >= 255) {
        flush_band(j);
        elen = brother_encode_line(bits, NULL, j->line_bytes, j->line_enc);
    }
    if (j->block_len + elen < BRL_MAX_BLOCK_BYTES) {
        memcpy(j->block + j->block_len, j->line_enc, elen);
        j->block_len += elen;
        j->block_lines++;
    }
    memcpy(j->ref, bits, (size_t)j->line_bytes);
    j->y++;
    return j->err ? -1 : 0;
}

static int page_line(void *job, int y, const uint8_t *px)
{
    br_job_t *j = job;
    if (j->err || !j->in_page)
        return -1;
    if (y >= j->height)
        return 0;
    mono_rgb_to_density(px, j->src_w, j->bytes_pp, j->density, j->width);
    mono_dither_1bit(j->density, j->width, y, j->cur, j->line_bytes);
    return emit_encoded_line(j, j->cur);
}

static int page_end(void *job)
{
    br_job_t *j = job;
    if (!j->in_page)
        return 0;
    memset(j->cur, 0, (size_t)j->line_bytes);
    while (!j->err && j->y < j->height)
        emit_encoded_line(j, j->cur);
    flush_band(j);
    br_write_str(j, "1030M\f");
    j->in_page = false;
    return j->err ? -1 : 0;
}

static int job_end(void *job)
{
    br_job_t *j = job;
    if (j->in_page && !j->err)
        page_end(j);
    if (!j->err) {
        br_write_str(j, "\x1b%-12345X@PJL\n@PJL EOJ NAME=\"AirPrint\"\n\x1b%-12345X\n");
        if (!j->err && j->io.flush && j->io.flush(j->io.ctx) < 0)
            j->err = true;
    }
    int ret = j->err ? -1 : 0;
    free(j->density);
    free(j->cur);
    free(j->ref);
    free(j->line_enc);
    free(j->block);
    free(j);
    return ret;
}

static int test_page(const printer_driver_t *d, const printer_io_t *io, int dpi, const char *label)
{
    br_job_t *j = job_begin(d, io, dpi, 0);
    if (!j)
        return -1;
    if (j->err || page_begin(j, j->width, j->height, 1) != 0) {
        job_end(j);
        return -1;
    }
    for (int y = 0; y < j->height && !j->err; y++) {
        mono_testpage_line(y, j->width, j->height, dpi, d->make_model, label, j->cur, j->line_bytes);
        if (emit_encoded_line(j, j->cur) != 0)
            break;
        if ((y & 63) == 63 && io->progress)
            io->progress(io->ctx, y + 1, j->height);
    }
    if (!j->err)
        page_end(j);
    return job_end(j);
}

// ---------------------------------------------------------------------------
static const printer_paper_t BR_PAPERS[] = {
    {"A4", "iso_a4_210x297mm", 21000, 29700},
    {"Letter", "na_letter_8.5x11in", 21590, 27940},
};

#define BROTHER_LASER_EX(var, id_, mdl, pid, duplex_)                                                                  \
    const printer_driver_t var = {                                                                                     \
        .id = id_,                                                                                                     \
        .make_model = "Brother " mdl,                                                                                  \
        .usb_mfg = "Brother",                                                                                          \
        .usb_mdl = mdl,                                                                                                \
        .usb_vid = 0x04f9,                                                                                             \
        .usb_pid = pid,                                                                                                \
        .color = false,                                                                                                \
        .scanner = false,                                                                                              \
        .duplex = duplex_,                                                                                             \
        .resolutions = {300, 600},                                                                                     \
        .nresolutions = 2,                                                                                             \
        .papers = BR_PAPERS,                                                                                           \
        .npapers = 2,                                                                                                  \
        .margin_left = 423,                                                                                            \
        .margin_right = 423,                                                                                           \
        .margin_top = 423,                                                                                             \
        .margin_bottom = 423,                                                                                          \
        .model = NULL,                                                                                                 \
        .job_begin = job_begin,                                                                                        \
        .page_begin = page_begin,                                                                                      \
        .page_line = page_line,                                                                                        \
        .page_end = page_end,                                                                                          \
        .job_end = job_end,                                                                                            \
        .test_page = test_page,                                                                                        \
    }

#define BROTHER_LASER(var, id_, mdl, pid) BROTHER_LASER_EX(var, id_, mdl, pid, false)

BROTHER_LASER(brother_hl1110_driver, "brother-hl1110", "HL-1110 series", 0x0054);
BROTHER_LASER(brother_hl1210w_driver, "brother-hl1210w", "HL-1210W series", 0x006a);
BROTHER_LASER(brother_hl2130_driver, "brother-hl2130", "HL-2130 series", 0x003f);
BROTHER_LASER(brother_hl2240_driver, "brother-hl2240", "HL-2240 series", 0x0040);
BROTHER_LASER_EX(brother_hll2300d_driver, "brother-hll2300d", "HL-L2300D series", 0x0061, true);
BROTHER_LASER(brother_dcp1510_driver, "brother-dcp1510", "DCP-1510 series", 0x02d0);
BROTHER_LASER(brother_dcp1610w_driver, "brother-dcp1610w", "DCP-1610W series", 0x035b);
BROTHER_LASER_EX(brother_dcpl2520d_driver, "brother-dcpl2520d", "DCP-L2520D series", 0x0320, true);
BROTHER_LASER(brother_generic_driver, "brother-laser", "Laser", 0x0000);
