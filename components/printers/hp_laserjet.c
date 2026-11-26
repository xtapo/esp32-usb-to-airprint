// HP LaserJet monochrome driver (ZjStream and XQX protocols).
// Uses clean-room ITU-T T.82 JBIG1 encoding (jbig1_enc.c) and 8x8 Bayer
// dithering (mono_raster.c). Supports 23 HP LaserJet ZjStream & XQX models.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jbig1_enc.h"
#include "mono_raster.h"
#include "printer_driver.h"

typedef enum {
    HP_PROTO_ZJS = 0,       // Zenographics ZjStream ("JZJZ")
    HP_PROTO_ZJS_PJL,       // ZjStream wrapped in PJL (P1102, P1566, P1606dn, M12)
    HP_PROTO_XQX,           // XQX (",XQX") wrapped in PJL
} hp_proto_t;

typedef struct {
    hp_proto_t proto;
} hp_model_t;

typedef struct {
    uint8_t *buf;
    size_t len;
    size_t cap;
    bool err;
} hp_buf_t;

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
    uint8_t *bits;
    jbig1_enc_t jbig;
    bool jbig_active;
    hp_buf_t stripe_buf;    // collects JBIG1 bytes for the current page
    int pages;
    bool err;
} hp_job_t;

static void write_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void write_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static int io_write_all(hp_job_t *j, const void *data, size_t len)
{
    if (j->err)
        return -1;
    if (j->io.write(j->io.ctx, (const uint8_t *)data, len) < 0) {
        j->err = true;
        return -1;
    }
    return 0;
}

static int io_write_str(hp_job_t *j, const char *s)
{
    return io_write_all(j, s, strlen(s));
}

// --- ZjStream helpers (big-endian) -----------------------------------------
enum {
    ZJT_START_DOC = 0,
    ZJT_END_DOC = 1,
    ZJT_START_PAGE = 2,
    ZJT_END_PAGE = 3,
    ZJT_JBIG_BIH = 4,
    ZJT_JBIG_BID = 5,
    ZJT_END_JBIG = 6,
};

enum {
    ZJI_DMCOLLATE = 1,
    ZJI_DMDUPLEX = 2,
    ZJI_DMPAPER = 3,
    ZJI_DMCOPIES = 4,
    ZJI_DMDEFAULTSOURCE = 5,
    ZJI_DMMEDIATYPE = 6,
    ZJI_NBIE = 7,
    ZJI_RESOLUTION_X = 8,
    ZJI_RESOLUTION_Y = 9,
    ZJI_OFFSET_X = 10,
    ZJI_OFFSET_Y = 11,
    ZJI_RASTER_X = 12,
    ZJI_RASTER_Y = 13,
    ZJI_VIDEO_BPP = 16,
    ZJI_VIDEO_X = 17,
    ZJI_VIDEO_Y = 18,
    ZJI_ECONOMODE = 23,
    ZJI_PAGECOUNT = 0,
};

static int zjs_chunk_hdr(hp_job_t *j, uint32_t type, uint32_t items, uint32_t payload_bytes)
{
    uint8_t h[16];
    write_be32(h + 0, 16u + payload_bytes);
    write_be32(h + 4, type);
    write_be32(h + 8, items);
    write_be16(h + 12, 0);
    write_be16(h + 14, 0x5a5au);  // 'ZZ'
    return io_write_all(j, h, sizeof(h));
}

static int zjs_item_u32(hp_job_t *j, uint16_t id, uint32_t val)
{
    uint8_t it[12];
    write_be32(it + 0, 12u);
    write_be16(it + 4, id);
    it[6] = 1;  // ZJIT_UINT32
    it[7] = 0;
    write_be32(it + 8, val);
    return io_write_all(j, it, sizeof(it));
}

// --- XQX helpers (big-endian) ----------------------------------------------
enum {
    XQX_START_DOC = 1,
    XQX_END_DOC = 2,
    XQX_START_PAGE = 3,
    XQX_END_PAGE = 4,
    XQX_START_PLANE = 5,
    XQX_END_PLANE = 6,
    XQX_JBIG = 7,
};

enum {
    XQXI_DMMEDIATYPE = 0x20000000u,
    XQXI_DMPAPER = 0x20000001u,
    XQXI_DMDEFAULTSOURCE = 0x20000006u,
    XQXI_RESOLUTION_X = 0x20000008u,
    XQXI_RESOLUTION_Y = 0x20000009u,
    XQXI_VIDEO_BPP = 0x2000000au,
    XQXI_RASTER_X = 0x2000000du,
    XQXI_RASTER_Y = 0x2000000eu,
    XQXI_VIDEO_X = 0x2000000fu,
    XQXI_VIDEO_Y = 0x20000010u,
    XQXI_ECONOMODE = 0x20000011u,
    XQXI_DMDUPLEX = 0x10000002u,
    XQXI_BIH = 0x40000002u,
    XQXI_END = 0x80000001u,
};

static int xqx_hdr(hp_job_t *j, uint32_t type, uint32_t val)
{
    uint8_t h[8];
    write_be32(h + 0, type);
    write_be32(h + 4, val);
    return io_write_all(j, h, sizeof(h));
}

static int xqx_item_u32(hp_job_t *j, uint32_t type, uint32_t val)
{
    uint8_t it[12];
    write_be32(it + 0, type);
    write_be32(it + 4, 4u);
    write_be32(it + 8, val);
    return io_write_all(j, it, sizeof(it));
}

static int xqx_item_bytes(hp_job_t *j, uint32_t type, const uint8_t *data, uint32_t len)
{
    uint8_t hdr[8];
    write_be32(hdr + 0, type);
    write_be32(hdr + 4, len);
    if (io_write_all(j, hdr, sizeof(hdr)) < 0)
        return -1;
    return io_write_all(j, data, len);
}

// --- JBIG1 buffered sink ---------------------------------------------------
static int jbig_mem_sink(void *user, const uint8_t *data, size_t len)
{
    hp_buf_t *b = user;
    if (b->err)
        return -1;
    if (b->len + len > b->cap) {
        size_t ncap = b->cap ? b->cap * 2 : 16384;
        while (ncap < b->len + len)
            ncap *= 2;
        uint8_t *nb = realloc(b->buf, ncap);
        if (!nb) {
            b->err = true;
            return -1;
        }
        b->buf = nb;
        b->cap = ncap;
    }
    memcpy(b->buf + b->len, data, len);
    b->len += len;
    return 0;
}

static int dmpaper_for(int paper)
{
    return paper == 1 ? 1 /* DMPAPER_LETTER */ : 9 /* DMPAPER_A4 */;
}

static hp_proto_t proto_of(const printer_driver_t *d)
{
    return ((const hp_model_t *)d->model)->proto;
}

// Compute printable pixel geometry from driver paper & margins
static void page_dims(const printer_driver_t *d, int dpi, int paper, int *w_out, int *h_out)
{
    const printer_paper_t *pp = &d->papers[paper];
    int pw = (int)((long)pp->width * dpi / 2540);
    int ph = (int)((long)pp->height * dpi / 2540);
    int ml = (int)((long)d->margin_left * dpi / 2540);
    int mr = (int)((long)d->margin_right * dpi / 2540);
    int mt = (int)((long)d->margin_top * dpi / 2540);
    int mb = (int)((long)d->margin_bottom * dpi / 2540);
    int w = ((pw - ml - mr) + 31) & ~31;   // align raster width to 32-bit boundary
    int h = ph - mt - mb;
    *w_out = w;
    *h_out = h;
}

static void *job_begin(const printer_driver_t *d, const printer_io_t *io, int dpi, int paper)
{
    hp_job_t *j = calloc(1, sizeof(*j));
    if (!j)
        return NULL;
    j->drv = d;
    j->io = *io;
    j->dpi = dpi;
    j->paper = paper;
    page_dims(d, dpi, paper, &j->width, &j->height);
    j->line_bytes = (j->width + 7) / 8;
    j->density = malloc((size_t)j->width);
    j->bits = malloc((size_t)j->line_bytes);
    if (!j->density || !j->bits) {
        free(j->density);
        free(j->bits);
        free(j);
        return NULL;
    }

    hp_proto_t proto = proto_of(d);
    if (proto == HP_PROTO_ZJS_PJL) {
        io_write_str(j, "\x1b%-12345X@PJL JOB\n"
                        "@PJL SET JAMRECOVERY=OFF\n"
                        "@PJL SET DENSITY=3\n"
                        "@PJL SET ECONOMODE=OFF\n"
                        "@PJL SET RET=MEDIUM\n"
                        "@PJL ENTER LANGUAGE=ACL\n"
                        "\x1b%-12345X");
    } else if (proto == HP_PROTO_XQX) {
        io_write_str(j, "\x1b%-12345X@PJL JOB\n"
                        "@PJL SET JAMRECOVERY=OFF\n"
                        "@PJL SET DENSITY=3\n"
                        "@PJL SET ECONOMODE=OFF\n"
                        "@PJL SET RET=MEDIUM\n"
                        "@PJL ENTER LANGUAGE=XQX\n"
                        "\x1b%-12345X");
    }

    int sides = d->duplex ? io->sides : PRINTER_SIDES_ONE_SIDED;
    if (proto == HP_PROTO_XQX) {
        uint32_t xqx_dup = sides == PRINTER_SIDES_TWO_SIDED_LONG_EDGE    ? 1
                         : sides == PRINTER_SIDES_TWO_SIDED_SHORT_EDGE ? 2
                                                                       : 0;
        io_write_all(j, ",XQX", 4);
        xqx_hdr(j, XQX_START_DOC, 7);
        xqx_item_u32(j, 0x80000000u, 84);
        xqx_item_u32(j, 0x10000005u, 1);
        xqx_item_u32(j, 0x10000001u, 0);
        xqx_item_u32(j, XQXI_DMDUPLEX, xqx_dup);
        xqx_item_u32(j, 0x10000000u, 0);
        xqx_item_u32(j, 0x10000003u, 1);
        xqx_item_u32(j, XQXI_END, 0xdeadbeefu);
    } else {
        uint32_t zjs_dup = sides == PRINTER_SIDES_TWO_SIDED_LONG_EDGE    ? 2
                         : sides == PRINTER_SIDES_TWO_SIDED_SHORT_EDGE ? 3
                                                                       : 1;
        io_write_all(j, "JZJZ", 4);
        zjs_chunk_hdr(j, ZJT_START_DOC, 3, 3u * 12u);
        zjs_item_u32(j, ZJI_DMCOLLATE, 0);
        zjs_item_u32(j, ZJI_DMDUPLEX, zjs_dup);
        zjs_item_u32(j, ZJI_PAGECOUNT, 0);
    }
    return j;
}

static int page_begin(void *job, int src_w, int src_h, int bytes_pp)
{
    hp_job_t *j = job;
    if (j->err)
        return -1;
    j->src_w = src_w;
    j->src_h = src_h;
    j->bytes_pp = bytes_pp;
    j->stripe_buf.len = 0;
    j->stripe_buf.err = false;
    if (jbig1_enc_init(&j->jbig, (uint32_t)j->width, (uint32_t)j->height, 128, jbig_mem_sink, &j->stripe_buf) != 0) {
        j->err = true;
        return -1;
    }
    j->jbig_active = true;
    j->pages++;

    int dmpaper = dmpaper_for(j->paper);
    if (proto_of(j->drv) == HP_PROTO_XQX) {
        xqx_hdr(j, XQX_START_PAGE, 15);
        xqx_item_u32(j, 0x80000000u, 180);
        xqx_item_u32(j, 0x20000005u, 1);
        xqx_item_u32(j, XQXI_DMDEFAULTSOURCE, 7);   // auto
        xqx_item_u32(j, XQXI_DMMEDIATYPE, 1);        // plain
        xqx_item_u32(j, 0x20000007u, 1);
        xqx_item_u32(j, XQXI_RESOLUTION_X, (uint32_t)j->dpi);
        xqx_item_u32(j, XQXI_RESOLUTION_Y, (uint32_t)j->dpi);
        xqx_item_u32(j, XQXI_RASTER_X, (uint32_t)j->width);
        xqx_item_u32(j, XQXI_RASTER_Y, (uint32_t)j->height);
        xqx_item_u32(j, XQXI_VIDEO_BPP, 1);
        xqx_item_u32(j, XQXI_VIDEO_X, (uint32_t)j->width);
        xqx_item_u32(j, XQXI_VIDEO_Y, (uint32_t)j->height);
        xqx_item_u32(j, XQXI_ECONOMODE, 0);
        xqx_item_u32(j, XQXI_DMPAPER, (uint32_t)dmpaper);
        xqx_item_u32(j, XQXI_END, 0xdeadbeefu);
    } else {
        zjs_chunk_hdr(j, ZJT_START_PAGE, 14, 14u * 12u);
        zjs_item_u32(j, ZJI_DMMEDIATYPE, 1);
        zjs_item_u32(j, ZJI_DMPAPER, (uint32_t)dmpaper);
        zjs_item_u32(j, ZJI_DMCOPIES, 1);
        zjs_item_u32(j, ZJI_DMDEFAULTSOURCE, 7);
        zjs_item_u32(j, ZJI_NBIE, 1);
        zjs_item_u32(j, ZJI_RESOLUTION_X, (uint32_t)j->dpi);
        zjs_item_u32(j, ZJI_RESOLUTION_Y, (uint32_t)j->dpi);
        zjs_item_u32(j, ZJI_OFFSET_X, 0);
        zjs_item_u32(j, ZJI_OFFSET_Y, 0);
        zjs_item_u32(j, ZJI_RASTER_X, (uint32_t)j->width);
        zjs_item_u32(j, ZJI_RASTER_Y, (uint32_t)j->height);
        zjs_item_u32(j, ZJI_VIDEO_BPP, 1);
        zjs_item_u32(j, ZJI_VIDEO_X, (uint32_t)j->width);
        zjs_item_u32(j, ZJI_VIDEO_Y, (uint32_t)j->height);
    }
    return j->err ? -1 : 0;
}

static int page_line(void *job, int y, const uint8_t *px)
{
    hp_job_t *j = job;
    if (j->err || !j->jbig_active)
        return -1;
    if (y >= j->height)
        return 0;
    mono_rgb_to_density(px, j->src_w, j->bytes_pp, j->density, j->width);
    mono_dither_1bit(j->density, j->width, y, j->bits, j->line_bytes);
    if (jbig1_enc_add_line(&j->jbig, j->bits) != 0) {
        j->err = true;
        return -1;
    }
    return 0;
}

static int flush_jbig_page(hp_job_t *j)
{
    if (!j->jbig_active)
        return 0;
    if (jbig1_enc_finish(&j->jbig) != 0 || j->stripe_buf.err) {
        j->err = true;
        jbig1_enc_free(&j->jbig);
        j->jbig_active = false;
        return -1;
    }
    uint8_t bih[20];
    jbig1_enc_get_bih(&j->jbig, bih);
    jbig1_enc_free(&j->jbig);
    j->jbig_active = false;

    const uint8_t *sdata = j->stripe_buf.buf;
    size_t slen = j->stripe_buf.len;

    if (proto_of(j->drv) == HP_PROTO_XQX) {
        xqx_hdr(j, XQX_START_PLANE, 4);
        xqx_item_u32(j, 0x80000000u, 64);
        xqx_item_u32(j, 0x40000000u, 0);
        xqx_item_bytes(j, XQXI_BIH, bih, 20);
        xqx_item_u32(j, XQXI_END, 0xdeadbeefu);

        xqx_hdr(j, XQX_JBIG, (uint32_t)slen);
        if (slen)
            io_write_all(j, sdata, slen);
        xqx_hdr(j, XQX_END_PLANE, 0);
        xqx_hdr(j, XQX_END_PAGE, 0);
    } else {
        zjs_chunk_hdr(j, ZJT_JBIG_BIH, 0, 20);
        io_write_all(j, bih, 20);

        size_t off = 0;
        while (off < slen && !j->err) {
            size_t chunk = slen - off;
            if (chunk > 65536)
                chunk = 65536;
            size_t pad = (4u - (chunk & 3u)) & 3u;
            zjs_chunk_hdr(j, ZJT_JBIG_BID, 0, (uint32_t)(chunk + pad));
            io_write_all(j, sdata + off, chunk);
            if (pad) {
                static const uint8_t zeros[4] = {0};
                io_write_all(j, zeros, pad);
            }
            off += chunk;
        }
        zjs_chunk_hdr(j, ZJT_END_JBIG, 0, 0);
        zjs_chunk_hdr(j, ZJT_END_PAGE, 0, 0);
    }
    return j->err ? -1 : 0;
}

static int page_end(void *job)
{
    return flush_jbig_page((hp_job_t *)job);
}

static int job_end(void *job)
{
    hp_job_t *j = job;
    if (j->jbig_active && !j->err)
        flush_jbig_page(j);
    if (!j->err) {
        hp_proto_t proto = proto_of(j->drv);
        if (proto == HP_PROTO_XQX) {
            xqx_hdr(j, XQX_END_DOC, 0);
            io_write_str(j, "\x1b%-12345X@PJL EOJ\n\x1b%-12345X");
        } else {
            zjs_chunk_hdr(j, ZJT_END_DOC, 0, 0);
            if (proto == HP_PROTO_ZJS_PJL)
                io_write_str(j, "\x1b%-12345X@PJL EOJ\n\x1b%-12345X");
        }
        if (!j->err && j->io.flush)
            if (j->io.flush(j->io.ctx) < 0)
                j->err = true;
    }
    int ret = j->err ? -1 : 0;
    if (j->jbig_active)
        jbig1_enc_free(&j->jbig);
    free(j->stripe_buf.buf);
    free(j->density);
    free(j->bits);
    free(j);
    return ret;
}

static int test_page(const printer_driver_t *d, const printer_io_t *io, int dpi, const char *label)
{
    hp_job_t *j = job_begin(d, io, dpi, 0);
    if (!j)
        return -1;
    if (j->err || page_begin(j, j->width, j->height, 1) != 0) {
        job_end(j);
        return -1;
    }
    for (int y = 0; y < j->height && !j->err; y++) {
        mono_testpage_line(y, j->width, j->height, dpi, d->make_model, label, j->bits, j->line_bytes);
        if (jbig1_enc_add_line(&j->jbig, j->bits) != 0) {
            j->err = true;
            break;
        }
        if ((y & 63) == 63 && io->progress)
            io->progress(io->ctx, y + 1, j->height);
    }
    if (!j->err)
        page_end(j);
    return job_end(j);
}

// ---------------------------------------------------------------------------
static const printer_paper_t HP_PAPERS[] = {
    {"A4", "iso_a4_210x297mm", 21000, 29700},
    {"Letter", "na_letter_8.5x11in", 21590, 27940},
};

static const hp_model_t HP_MODEL_ZJS = {HP_PROTO_ZJS};
static const hp_model_t HP_MODEL_ZJS_PJL = {HP_PROTO_ZJS_PJL};
static const hp_model_t HP_MODEL_XQX = {HP_PROTO_XQX};

#define HP_LASERJET_EX(var, id_, mdl, pid, duplex_, model_ptr)                                                         \
    const printer_driver_t var = {                                                                                     \
        .id = id_,                                                                                                     \
        .make_model = mdl,                                                                                             \
        .usb_mfg = "Hewlett-Packard",                                                                                  \
        .usb_mdl = mdl,                                                                                                \
        .usb_vid = 0x03f0,                                                                                             \
        .usb_pid = pid,                                                                                                \
        .color = false,                                                                                                \
        .scanner = false,                                                                                              \
        .duplex = duplex_,                                                                                             \
        .resolutions = {600},                                                                                          \
        .nresolutions = 1,                                                                                             \
        .papers = HP_PAPERS,                                                                                           \
        .npapers = 2,                                                                                                  \
        .margin_left = 423,                                                                                            \
        .margin_right = 423,                                                                                           \
        .margin_top = 423,                                                                                             \
        .margin_bottom = 423,                                                                                          \
        .model = model_ptr,                                                                                            \
        .job_begin = job_begin,                                                                                        \
        .page_begin = page_begin,                                                                                      \
        .page_line = page_line,                                                                                        \
        .page_end = page_end,                                                                                          \
        .job_end = job_end,                                                                                            \
        .test_page = test_page,                                                                                        \
    }

#define HP_LASERJET(var, id_, mdl, pid, model_ptr) HP_LASERJET_EX(var, id_, mdl, pid, false, model_ptr)

// ZjStream models
HP_LASERJET(hp_lj1000_driver, "hp-lj1000", "HP LaserJet 1000", 0x0517, &HP_MODEL_ZJS);
HP_LASERJET(hp_lj1005_driver, "hp-lj1005", "HP LaserJet 1005", 0x1317, &HP_MODEL_ZJS);
HP_LASERJET(hp_lj1018_driver, "hp-lj1018", "HP LaserJet 1018", 0x4117, &HP_MODEL_ZJS);
HP_LASERJET(hp_lj1020_driver, "hp-lj1020", "HP LaserJet 1020", 0x2b17, &HP_MODEL_ZJS);
HP_LASERJET(hp_lj1022_driver, "hp-lj1022", "HP LaserJet 1022", 0x2c17, &HP_MODEL_ZJS);
HP_LASERJET(hp_m1319_driver, "hp-m1319", "HP LaserJet M1319f MFP", 0x4c17, &HP_MODEL_ZJS);
HP_LASERJET(hp_p1102_driver, "hp-p1102", "HP LaserJet Professional P1102", 0x002a, &HP_MODEL_ZJS_PJL);
HP_LASERJET(hp_p1102w_driver, "hp-p1102w", "HP LaserJet Professional P1102w", 0x032a, &HP_MODEL_ZJS_PJL);
HP_LASERJET(hp_p1566_driver, "hp-p1566", "HP LaserJet Professional P1566", 0x082a, &HP_MODEL_ZJS_PJL);
HP_LASERJET_EX(hp_p1606dn_driver, "hp-p1606dn", "HP LaserJet Professional P1606dn", 0x092a, true, &HP_MODEL_ZJS_PJL);
HP_LASERJET(hp_m12_driver, "hp-m12", "HP LaserJet Pro M12a", 0x542a, &HP_MODEL_ZJS_PJL);

// XQX models
HP_LASERJET(hp_p1005_driver, "hp-p1005", "HP LaserJet P1005", 0x3d17, &HP_MODEL_XQX);
HP_LASERJET(hp_p1006_driver, "hp-p1006", "HP LaserJet P1006", 0x3e17, &HP_MODEL_XQX);
HP_LASERJET(hp_p1007_driver, "hp-p1007", "HP LaserJet P1007", 0x4817, &HP_MODEL_XQX);
HP_LASERJET(hp_p1008_driver, "hp-p1008", "HP LaserJet P1008", 0x4917, &HP_MODEL_XQX);
HP_LASERJET(hp_p1009_driver, "hp-p1009", "HP LaserJet P1009", 0x4a17, &HP_MODEL_XQX);
HP_LASERJET(hp_p1505_driver, "hp-p1505", "HP LaserJet P1505", 0x3f17, &HP_MODEL_XQX);
HP_LASERJET(hp_p2014_driver, "hp-p2014", "HP LaserJet P2014", 0x4617, &HP_MODEL_XQX);
HP_LASERJET(hp_m1005_driver, "hp-m1005", "HP LaserJet M1005", 0x3b17, &HP_MODEL_XQX);
HP_LASERJET(hp_m1120_driver, "hp-m1120", "HP LaserJet M1120 MFP", 0x5617, &HP_MODEL_XQX);
HP_LASERJET(hp_m1132_driver, "hp-m1132", "HP LaserJet Professional M1132 MFP", 0x042a, &HP_MODEL_XQX);
HP_LASERJET(hp_m1136_driver, "hp-m1136", "HP LaserJet Professional M1136 MFP", 0x052a, &HP_MODEL_XQX);
HP_LASERJET(hp_m1212nf_driver, "hp-m1212nf", "HP LaserJet Professional M1212nf MFP", 0x062a, &HP_MODEL_XQX);
