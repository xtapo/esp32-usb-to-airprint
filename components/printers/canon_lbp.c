// Canon LBP CAPT (Canon Advanced Printing Technology) + Hi-SCoA driver.
// Supports Canon LBP2900, LBP3000, LBP3050, LBP3300, LBP6018L, LBP6030, LBP6040.
// Clean-room implementation following documented CAPT/Hi-SCoA wire formats.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#define CAPT_LOGI(...) ESP_LOGI("capt", __VA_ARGS__)
#define CAPT_LOGW(...) ESP_LOGW("capt", __VA_ARGS__)
#else
#define CAPT_LOGI(...) ((void)0)
#define CAPT_LOGW(...) ((void)0)
#endif

#include "mono_raster.h"
#include "printer_driver.h"

typedef enum {
    CAPT_LBP2900 = 0,
    CAPT_LBP3000,
    CAPT_LBP3050,
    CAPT_LBP3300,
    CAPT_LBP6000,       // LBP6018L, LBP6030, LBP6040
} capt_variant_t;

typedef struct {
    capt_variant_t variant;
} capt_model_t;

enum {
    CAPT_NOP = 0xA0A0,
    CAPT_CHKJOBSTAT = 0xA0A1,
    CAPT_CHKXSTATUS = 0xA0A8,
    CAPT_IDENT = 0xA1A1,
    CAPT_JOB_BEGIN = 0xA2A0,
    CAPT_START_0 = 0xA3A2,
    CAPT_PRINT_DATA = 0xC0A0,
    CAPT_PRINT_DATA_END = 0xC0A4,
    CAPT_SET_PARM_PAGE = 0xD0A0,
    CAPT_SET_PARM_1 = 0xD0A1,
    CAPT_SET_PARM_2 = 0xD0A2,
    CAPT_SET_PARM_HISCOA = 0xD0A4,
    CAPT_SET_PARMS = 0xD0A9,
    CAPT_CHKSTATUS = 0xE0A0,
    CAPT_START_2 = 0xE0A2,
    CAPT_START_1 = 0xE0A3,
    CAPT_START_3 = 0xE0A4,
    CAPT_UPLOAD_2 = 0xE0A5,
    CAPT_LBP3000_SETUP_0 = 0xE0A6,
    CAPT_FIRE = 0xE0A7,
    CAPT_JOB_END = 0xE0A9,
    CAPT_LBP6000_SETUP_0 = 0xE0BA,
    CAPT_JOB_SETUP = 0xE1A1,
    CAPT_GPIO = 0xE1A2,
    CAPT_SELECT_INPUT = 0xE1A9,
};

#define CAPT_BAND_LINES 70

typedef struct {
    const printer_driver_t *drv;
    printer_io_t io;
    int dpi;
    int paper;
    int sides;
    int width;
    int height;
    int line_bytes;
    int paper_w;
    int paper_h;
    int margin_left;
    int margin_top;
    int src_w;
    int src_h;
    int bytes_pp;

    uint16_t job_id;
    uint16_t page_num;
    uint16_t hw_page;
    uint16_t page_decoding;
    uint16_t page_received;
    uint16_t page_out;
    uint16_t page_completed;
    uint16_t status0;
    uint16_t status1;
    uint16_t status2;
    uint16_t eng_status;
    uint32_t bands_sent;

    uint8_t *density;
    uint8_t *band;          // CAPT_BAND_LINES * line_bytes
    uint8_t *comp;          // compressed band buffer
    size_t comp_cap;
    int band_y;             // lines buffered in band
    int total_y;            // lines received on current page
    bool in_page;
    bool err;
    bool io_err;
    uint8_t model_id[2];
    bool has_model_id;
    uint8_t fifo_free;
    bool has_fifo_credits;
    bool page_fired;
    uint32_t page_bytes_sent;
} capt_job_t;

typedef struct {
    volatile bool valid;
    capt_variant_t variant;
    uint16_t status0;
    uint16_t status1;
    uint16_t status2;
    uint16_t eng_status;
    uint8_t r26;
    uint16_t page_out;
    uint16_t page_completed;
} capt_live_status_t;

static capt_live_status_t s_capt_status;

static void capt_log_hex(const char *label, const uint8_t *buf, size_t len)
{
#ifdef ESP_PLATFORM
    for (size_t off = 0; off < len && off < 192; off += 48) {
        char hex[48 * 3 + 1];
        size_t n = len - off > 48 ? 48 : len - off;
        size_t pos = 0;
        for (size_t i = 0; i < n; i++)
            pos += (size_t)snprintf(hex + pos, sizeof(hex) - pos, i + 1 < n ? "%02x " : "%02x", buf[off + i]);
        hex[pos] = 0;
        CAPT_LOGI("%s[%u] (%u B): %s", label, (unsigned)off, (unsigned)len, hex);
    }
#else
    (void)label;
    (void)buf;
    (void)len;
#endif
}

static void capt_sleep_ms(int ms)
{
#ifdef ESP_PLATFORM
    vTaskDelay(pdMS_TO_TICKS(ms));
#else
    (void)ms;
#endif
}

static int capt_write_all(capt_job_t *j, const void *data, size_t len)
{
    if (j->err)
        return -1;
    if (j->io.write(j->io.ctx, (const uint8_t *)data, len) < 0) {
        j->err = true;
        j->io_err = true;
        return -1;
    }
    return 0;
}

static int capt_send_pkt(capt_job_t *j, uint16_t cmd, const void *payload, size_t plen)
{
    uint8_t hdr[4] = {
        (uint8_t)(cmd & 0xff),
        (uint8_t)(cmd >> 8),
        (uint8_t)((4 + plen) & 0xff),
        (uint8_t)((4 + plen) >> 8),
    };
    if (capt_write_all(j, hdr, 4) < 0)
        return -1;
    if (plen > 0 && capt_write_all(j, payload, plen) < 0)
        return -1;
    return 0;
}

static int capt_read_exact(capt_job_t *j, uint8_t *buf, size_t need, int timeout_ms)
{
    size_t got = 0;
    int retries = 0;
    while (got < need) {
        int r = j->io.read(j->io.ctx, buf + got, need - got, timeout_ms);
        if (r < 0)
            return -1;
        if (r == 0) {
            if (++retries > 3)
                return -1;
            continue;
        }
        got += (size_t)r;
    }
    return (int)got;
}

static bool capt_bcd_len(uint8_t lo, uint8_t hi, size_t *out)
{
    if ((lo & 0x0fu) >= 10 || (lo >> 4) >= 10 || (hi & 0x0fu) >= 10 || (hi >> 4) >= 10)
        return false;
    *out = (size_t)(lo & 0x0fu) + (size_t)(lo >> 4) * 10u + (size_t)(hi & 0x0fu) * 100u +
           (size_t)(hi >> 4) * 1000u;
    return true;
}

// Sends a CAPT command and optionally reads its reply when io.read != NULL.
static int capt_sendrecv(capt_job_t *j, uint16_t cmd, const void *payload, size_t plen, uint8_t *reply, size_t rcap,
                         size_t *rlen)
{
    if (rlen)
        *rlen = 0;
    if (capt_send_pkt(j, cmd, payload, plen) < 0)
        return -1;
    if (!j->io.read)
        return 0;
    if (j->io.flush && j->io.flush(j->io.ctx) < 0) {
        CAPT_LOGW("cmd %04x: flush failed", cmd);
        j->err = true;
        j->io_err = true;
        return -1;
    }
    uint8_t head[6];
    if (capt_read_exact(j, head, 6, 3000) < 0) {
        CAPT_LOGW("cmd %04x: header read failed/timeout", cmd);
        j->err = true;
        j->io_err = true;
        return -1;
    }
    size_t sz = (size_t)(head[2] | (head[3] << 8));
    size_t bcd_sz = 0;
    bool bcd_valid = capt_bcd_len(head[2], head[3], &bcd_sz) && bcd_sz >= 6 && bcd_sz < sz;
    if (sz < 6)
        sz = 6;
    if (reply && rcap > 0) {
        size_t first = rcap < 2 ? rcap : 2;
        memcpy(reply, head + 4, first);
    }

    size_t total = 6;
    int timeouts = 0;
    while (total < sz && (!bcd_valid || total != bcd_sz)) {
        uint8_t chunk[256];
        size_t want = sz - total;
        if (want > sizeof(chunk))
            want = sizeof(chunk);
        int r = j->io.read(j->io.ctx, chunk, want, 3000);
        if (r < 0) {
            CAPT_LOGW("cmd %04x: body read error at %zu/%zu", cmd, total, sz);
            j->err = true;
            j->io_err = true;
            return -1;
        }
        if (r == 0) {
            if (bcd_valid && total == bcd_sz)
                break;
            if (++timeouts > 2) {
                CAPT_LOGW("cmd %04x: body read timeout at %zu/%zu (bcd=%s%zu)", cmd, total, sz, bcd_valid ? "" : "no:",
                          bcd_sz);
                j->err = true;
                j->io_err = true;
                return -1;
            }
            continue;
        }
        timeouts = 0;
        if (reply && total - 4 < rcap) {
            size_t off = total - 4;
            size_t avail = rcap - off;
            size_t cp = (size_t)r < avail ? (size_t)r : avail;
            memcpy(reply + off, chunk, cp);
        }
        total += (size_t)r;
    }
    if (rlen)
        *rlen = total - 4;
    if (cmd != CAPT_CHKSTATUS && cmd != CAPT_CHKXSTATUS) {
        CAPT_LOGI("cmd %04x -> %zu B [%02x %02x %02x %02x %02x %02x]", cmd, total, head[0], head[1], head[2],
                  head[3], head[4], head[5]);
    }
    return 0;
}

static int capt_send_flush(capt_job_t *j, uint16_t cmd, const void *payload, size_t plen)
{
    if (capt_send_pkt(j, cmd, payload, plen) < 0)
        return -1;
    if (j->io.flush && j->io.flush(j->io.ctx) < 0) {
        CAPT_LOGW("cmd %04x: flush failed", cmd);
        j->err = true;
        j->io_err = true;
        return -1;
    }
    return 0;
}

static capt_variant_t variant_of(const printer_driver_t *d)
{
    return ((const capt_model_t *)d->model)->variant;
}

static void capt_record_xstatus(capt_variant_t v, const uint8_t *r, size_t rn)
{
    if (rn < 22)
        return;
    uint16_t s0 = (uint16_t)(r[0] | (r[1] << 8));
    uint16_t s1 = (uint16_t)(r[8] | (r[9] << 8));
    uint16_t s2 = (uint16_t)(r[10] | (r[11] << 8));
    uint16_t eng = (uint16_t)(r[12] | (r[13] << 8));
    uint8_t r26 = rn >= 27 ? r[26] : 0;
    uint16_t pout = (uint16_t)(r[18] | (r[19] << 8));
    uint16_t pcomp = (uint16_t)(r[20] | (r[21] << 8));
    if (!s_capt_status.valid || s_capt_status.variant != v || s_capt_status.status0 != s0 ||
        s_capt_status.status1 != s1 || s_capt_status.status2 != s2 || s_capt_status.eng_status != eng ||
        s_capt_status.r26 != r26) {
        CAPT_LOGI("status: s0=%04x s1=%04x s2=%04x eng=%04x r26=%02x out=%u comp=%u", s0, s1, s2, eng, r26, pout,
                  pcomp);
    }
    s_capt_status.variant = v;
    s_capt_status.status0 = s0;
    s_capt_status.status1 = s1;
    s_capt_status.status2 = s2;
    s_capt_status.eng_status = eng;
    s_capt_status.r26 = r26;
    s_capt_status.page_out = pout;
    s_capt_status.page_completed = pcomp;
    s_capt_status.valid = true;
}

static int capt_get_status(capt_job_t *j)
{
    if (!j->io.read)
        return 0;
    uint8_t r[64];
    size_t rn = 0;
    memset(r, 0, sizeof(r));
    if (capt_sendrecv(j, CAPT_CHKSTATUS, NULL, 0, r, sizeof(r), &rn) < 0)
        return -1;
    if (rn >= 2)
        j->status0 = (uint16_t)(r[0] | (r[1] << 8));
    return 0;
}

static int capt_get_xstatus(capt_job_t *j, bool force)
{
    if (!j->io.read)
        return 0;
    capt_variant_t v = variant_of(j->drv);
    if (!force && (v == CAPT_LBP2900 || v == CAPT_LBP3000 || v == CAPT_LBP3300)) {
        if (capt_get_status(j) < 0)
            return -1;
        if (!(j->status0 & 0x0100u))
            return 0;
    }
    uint8_t r[96];
    size_t rn = 0;
    memset(r, 0, sizeof(r));
    if (capt_sendrecv(j, CAPT_CHKXSTATUS, NULL, 0, r, sizeof(r), &rn) < 0)
        return -1;
    if (rn >= 6) {
        j->fifo_free = (uint8_t)(r[4] & 0x0Fu);
        if (j->fifo_free > 0)
            j->has_fifo_credits = true;
    }
    if (rn >= 22) {
        j->status0 = (uint16_t)(r[0] | (r[1] << 8));
        j->status1 = (uint16_t)(r[8] | (r[9] << 8));
        j->status2 = (uint16_t)(r[10] | (r[11] << 8));
        j->eng_status = (uint16_t)(r[12] | (r[13] << 8));
        j->page_decoding = (uint16_t)(r[14] | (r[15] << 8));
        j->page_out = (uint16_t)(r[18] | (r[19] << 8));
        j->page_completed = (uint16_t)(r[20] | (r[21] << 8));
        capt_record_xstatus(v, r, rn);
    }
    if (rn >= 36)
        j->page_received = (uint16_t)(r[34] | (r[35] << 8));
    return 0;
}

static int capt_poll_status(capt_job_t *j)
{
    return capt_get_xstatus(j, false);
}

static bool capt_has_hard_error(const capt_job_t *j)
{
    if (j->eng_status & (0x0100u | 0x4000u | 0x0002u))
        return true;    // jam, cover open / no cartridge, service call
    bool nopaper_bit = variant_of(j->drv) == CAPT_LBP3300
                           ? ((j->status1 & 0x4000u) != 0)
                           : (((j->status0 & 0x0002u) || (j->status1 & 0x4000u)) != 0);
    if (nopaper_bit && !(j->status1 & (0x0004u | 0x0080u)))
        return true;    // out of paper / print rejected and engine stopped
    return false;
}

static void capt_add_reason(airprint_status_t *st, const char *reason)
{
    for (int i = 0; i < st->nreasons; i++)
        if (strcmp(st->reasons[i], reason) == 0)
            return;
    if (st->nreasons < (int)(sizeof(st->reasons) / sizeof(st->reasons[0])))
        st->reasons[st->nreasons++] = reason;
}

static int capt_idle_poll_status(const printer_driver_t *d, const printer_io_t *io)
{
    if (!d || !io || !io->write || !io->read)
        return -1;
    static const uint8_t cmd[4] = {
        (uint8_t)(CAPT_CHKXSTATUS & 0xff), (uint8_t)(CAPT_CHKXSTATUS >> 8), 0x04, 0x00,
    };
    if (io->write(io->ctx, cmd, sizeof(cmd)) < 0) {
        CAPT_LOGW("idle poll: write failed");
        return -1;
    }
    if (io->flush && io->flush(io->ctx) < 0) {
        CAPT_LOGW("idle poll: flush failed");
        return -1;
    }

    uint8_t head[6];
    size_t got = 0;
    int retries = 0;
    while (got < sizeof(head)) {
        int r = io->read(io->ctx, head + got, sizeof(head) - got, 500);
        if (r < 0) {
            CAPT_LOGW("idle poll: header read err (%d) at %zu", r, got);
            return -1;
        }
        if (r == 0) {
            if (++retries > 1) {
                CAPT_LOGW("idle poll: header timeout at %zu", got);
                return -1;
            }
            continue;
        }
        got += (size_t)r;
    }

    size_t sz = (size_t)(head[2] | (head[3] << 8));
    size_t bcd_sz = 0;
    bool bcd_valid = capt_bcd_len(head[2], head[3], &bcd_sz) && bcd_sz >= 6 && bcd_sz < sz;
    if (sz < 6)
        sz = 6;

    uint8_t rbuf[96];
    memset(rbuf, 0, sizeof(rbuf));
    memcpy(rbuf, head + 4, 2);

    size_t total = 6;
    int timeouts = 0;
    while (total < sz && (!bcd_valid || total != bcd_sz)) {
        uint8_t chunk[96];
        size_t want = sz - total;
        if (want > sizeof(chunk))
            want = sizeof(chunk);
        int r = io->read(io->ctx, chunk, want, 500);
        if (r < 0) {
            CAPT_LOGW("idle poll: body read err (%d) at %zu/%zu [%02x %02x %02x %02x %02x %02x]", r, total, sz,
                      head[0], head[1], head[2], head[3], head[4], head[5]);
            return -1;
        }
        if (r == 0) {
            if (bcd_valid && total == bcd_sz)
                break;
            if (++timeouts > 1) {
                CAPT_LOGW("idle poll: body timeout at %zu/%zu [%02x %02x %02x %02x %02x %02x]", total, sz, head[0],
                          head[1], head[2], head[3], head[4], head[5]);
                return -1;
            }
            continue;
        }
        timeouts = 0;
        if (total - 4 < sizeof(rbuf)) {
            size_t off = total - 4;
            size_t avail = sizeof(rbuf) - off;
            size_t cp = (size_t)r < avail ? (size_t)r : avail;
            memcpy(rbuf + off, chunk, cp);
        }
        total += (size_t)r;
    }
    if (total - 4 < 22) {
        CAPT_LOGW("idle poll: short reply %zu B [%02x %02x %02x %02x %02x %02x]", total, head[0], head[1], head[2],
                  head[3], head[4], head[5]);
        return -1;
    }
    capt_record_xstatus(variant_of(d), rbuf, total - 4);
    return 0;
}

static void capt_status(const char *state, const char *detail, const char *data, airprint_status_t *st)
{
    (void)state;
    (void)detail;
    (void)data;
    if (!s_capt_status.valid)
        return;

    uint16_t s0 = s_capt_status.status0;
    uint16_t s1 = s_capt_status.status1;
    uint16_t s2 = s_capt_status.status2;
    uint16_t eng = s_capt_status.eng_status;
    uint8_t r26 = s_capt_status.r26;
    capt_variant_t v = s_capt_status.variant;

    if (eng & 0x0100u) {
        capt_add_reason(st, "media-jam-error");
        if (!st->message[0])
            snprintf(st->message, sizeof(st->message), "Paper jam");
    }
    if (eng & 0x4000u) {
        if (r26 != 0) {
            capt_add_reason(st, "marker-supply-empty-error");
            if (!st->message[0])
                snprintf(st->message, sizeof(st->message), "No toner cartridge");
        } else {
            capt_add_reason(st, "cover-open-error");
            if (!st->message[0])
                snprintf(st->message, sizeof(st->message), "Cover open");
        }
    }
    if (eng & 0x0002u) {
        capt_add_reason(st, "other-error");
        if (!st->message[0])
            snprintf(st->message, sizeof(st->message), "Printer service error");
    }

    bool no_paper_tray = (v == CAPT_LBP3300) && ((s2 & 0x00C0u) == 0);
    bool print_rejected = (s1 & 0x4000u) != 0;
    bool basic_nopaper = (v != CAPT_LBP3300) && ((s0 & 0x0002u) != 0) &&
                         !(eng & (0x0100u | 0x4000u | 0x0002u)) && !(s1 & 0x0084u);
    if (no_paper_tray || print_rejected || basic_nopaper) {
        capt_add_reason(st, "media-empty-error");
        if (!st->message[0])
            snprintf(st->message, sizeof(st->message), "Out of paper");
    }

    if (st->nreasons == 0 && (s1 & 0x1F00u) != 0) {
        capt_add_reason(st, "other-error");
        if (!st->message[0])
            snprintf(st->message, sizeof(st->message), "Printer error");
    }

    if (st->nreasons == 0 && !st->message[0]) {
        if (s1 & 0x0004u)
            snprintf(st->message, sizeof(st->message), "Printing");
        else if (eng & 0x0004u)
            snprintf(st->message, sizeof(st->message), "Cleaning");
        else if ((eng & 0x1000u) || (s0 & 0x0004u))
            snprintf(st->message, sizeof(st->message), "Warming up");
    }
}

static int capt_wait_ready(capt_job_t *j)
{
    if (!j->io.read)
        return 0;
    capt_variant_t v = variant_of(j->drv);
    bool basic = (v == CAPT_LBP2900 || v == CAPT_LBP3000 || v == CAPT_LBP3300);
    for (int t = 0; t < 120; t++) {
        if ((basic ? capt_get_status(j) : capt_get_xstatus(j, true)) < 0)
            return -1;
        if (!(j->status0 & 0x0080u))
            return 0;
        capt_sleep_ms(100);
    }
    CAPT_LOGW("wait_ready timeout s0=%04x", j->status0);
    return 0;
}

static int capt_send_job_setup(capt_job_t *j, uint8_t fg, uint16_t page)
{
    uint8_t buf[72];
    memset(buf, 0, sizeof(buf));
    buf[4] = (uint8_t)(page & 0xff);
    buf[5] = (uint8_t)(page >> 8);
    buf[16] = fg;
    buf[17] = 0x01;
    buf[18] = (uint8_t)(j->job_id & 0xff);
    buf[19] = (uint8_t)(j->job_id >> 8);
    buf[20] = 0xC4;
    buf[21] = 0xFF;
    buf[22] = 0x88;
    buf[23] = 0xFF;
    buf[24] = 0x7E;
    buf[25] = 0x00;
    buf[26] = 1;
    buf[27] = 1;
    buf[31] = 0x01;
    return capt_sendrecv(j, CAPT_JOB_SETUP, buf, sizeof(buf), NULL, 0, NULL);
}

static int capt_fire_page(capt_job_t *j, uint16_t pg)
{
    if (j->page_fired || j->err)
        return j->err ? -1 : 0;
    j->page_fired = true;
    if (capt_send_job_setup(j, 2, pg) < 0)
        return -1;
    capt_wait_ready(j);
    uint8_t fire[2] = {(uint8_t)(pg & 0xff), (uint8_t)(pg >> 8)};
    if (capt_sendrecv(j, CAPT_FIRE, fire, sizeof(fire), NULL, 0, NULL) < 0)
        return -1;
    capt_wait_ready(j);
    if (capt_send_job_setup(j, 6, pg) < 0)
        return -1;
    CAPT_LOGI("fired pg=%u (sent=%u B fifo=%u dec=%u rx=%u)", pg, (unsigned)j->page_bytes_sent, j->fifo_free,
              j->page_decoding, j->page_received);
    return j->err ? -1 : 0;
}

// --- Hi-SCoA compressor (clean-room per CAPT Hi-SCoA spec) ----------------
typedef struct {
    const uint8_t *in;
    size_t in_size;
    size_t pos;
    uint8_t *out;
    size_t out_bytes;
    size_t bitpos;
    unsigned line_size;
    uint8_t stash[16];
    unsigned nstash;
    int origin[6];
} hiscoa_enc_t;

static void hs_push_bits(hiscoa_enc_t *s, uint32_t bits, unsigned count)
{
    size_t max_bits = s->out_bytes * 8u;
    while (count > 0 && s->bitpos < max_bits) {
        size_t byte_i = s->bitpos >> 3;
        unsigned spc = 8u - (unsigned)(s->bitpos & 7u);
        unsigned cnt = count > spc ? spc : count;
        uint8_t mask = (uint8_t)((0xffu >> (8u - cnt)) << (spc - cnt));
        uint8_t val = (uint8_t)((bits >> (count - cnt)) << (spc - cnt));
        uint8_t x = s->out[byte_i] ^ 0x43u;
        x = (uint8_t)((x & ~mask) | (val & mask));
        s->out[byte_i] = (uint8_t)(x ^ 0x43u);
        s->bitpos += cnt;
        count -= cnt;
    }
}

static unsigned hs_msb_idx(unsigned v)
{
    unsigned b = 0;
    while (v > 1u) {
        v >>= 1;
        b++;
    }
    return b;
}

static unsigned hs_match(const hiscoa_enc_t *s, int diff)
{
    if (diff <= 0 || s->pos < (size_t)diff)
        return 0;
    size_t p = s->pos;
    while (p < s->in_size && p < s->pos + 127u && s->in[p] == s->in[p - (size_t)diff]) {
        p++;
        if ((p % s->line_size) == 0)
            break;
    }
    return (unsigned)(p - s->pos);
}

static void hs_stash_push(hiscoa_enc_t *s, uint8_t b)
{
    if (s->nstash < 16)
        s->nstash++;
    for (unsigned i = s->nstash - 1; i > 0; i--)
        s->stash[i] = s->stash[i - 1];
    s->stash[0] = b;
}

size_t canon_hiscoa_compress_band(const uint8_t *band, unsigned line_size, unsigned nlines, int eob_last, uint8_t *out,
                                  size_t out_cap)
{
    hiscoa_enc_t s = {
        .in = band,
        .in_size = (size_t)line_size * nlines,
        .out = out,
        .out_bytes = out_cap,
        .line_size = line_size,
        .origin = {(int)line_size, 0, (int)line_size - 5, 4, 384, 12},
    };
    // initialize output buffer to 0x00 ^ 0x43 so XOR writes work cleanly
    memset(out, 0x43, out_cap);

    while (s.pos < s.in_size) {
        int best_cmd = -1;
        unsigned best_len = 0;
        for (int cmd = 0; cmd < 6; cmd++) {
            if (cmd == 1 || s.origin[cmd] <= 0)
                continue;
            unsigned len = hs_match(&s, s.origin[cmd]);
            if (len > best_len) {
                best_len = len;
                best_cmd = cmd;
            }
        }
        if (best_len > 1) {
            hs_push_bits(&s, 0xfffffffeu, (unsigned)(best_cmd + 1));
            if (best_cmd == 2)
                hs_push_bits(&s, 0, 1);
            switch (best_len) {
            case 1:
                hs_push_bits(&s, 0x0, 2);
                break;
            case 2:
                hs_push_bits(&s, 0x3, 3);
                break;
            case 3:
                hs_push_bits(&s, 0x2, 3);
                break;
            default: {
                unsigned ord = hs_msb_idx(best_len);
                hs_push_bits(&s, 0xfffffffeu, ord);
                hs_push_bits(&s, ~best_len, ord);
                break;
            }
            }
            if (best_cmd == 2) {
                int t = s.origin[2];
                s.origin[2] = s.origin[0];
                s.origin[0] = t;
            } else if (best_cmd == 5) {
                int t = s.origin[5];
                s.origin[5] = s.origin[3];
                s.origin[3] = t;
            }
            s.pos += best_len;
            continue;
        }

        uint8_t byte = s.in[s.pos];
        bool found = false;
        for (unsigned i = 0; i < s.nstash; i++) {
            if (s.stash[i] == byte) {
                for (unsigned k = i; k > 0; k--)
                    s.stash[k] = s.stash[k - 1];
                s.stash[0] = byte;
                hs_push_bits(&s, 0x20u | ((15u - i) & 0xfu), 6);
                s.pos++;
                found = true;
                break;
            }
        }
        if (found)
            continue;

        hs_stash_push(&s, byte);
        if (byte == 0)
            hs_push_bits(&s, 0xfdu, 8);
        else
            hs_push_bits(&s, 0xd00u | byte, 12);
        s.pos++;
    }

    hs_push_bits(&s, 0xfeu, 8);
    hs_push_bits(&s, eob_last ? 1u : 0u, 2);
    hs_push_bits(&s, 0xffffffffu, 32u - (unsigned)(s.bitpos & 31u));
    return s.bitpos >> 3;
}

static int flush_band(capt_job_t *j, bool last_band)
{
    (void)last_band;
    if (j->band_y <= 0 || j->err)
        return j->err ? -1 : 0;
    size_t clen = canon_hiscoa_compress_band(j->band, (unsigned)j->line_bytes, (unsigned)j->band_y, 0,
                                             j->comp, j->comp_cap);
    size_t off = 0;
    while (off < clen && !j->err) {
        size_t chunk = clen - off;
        if (chunk > 0xff00u)
            chunk = 0xff00u;
        j->bands_sent++;
        if (j->io.read) {
            if (j->has_fifo_credits) {
                if (!j->page_fired && j->page_bytes_sent >= 512u * 1024u) {
                    capt_get_xstatus(j, true);
                    if (j->page_decoding >= j->hw_page)
                        capt_fire_page(j, j->hw_page);
                }
                for (int t = 0; t < 1200 && j->fifo_free == 0 && !j->err; t++) {
                    if (capt_get_xstatus(j, true) < 0)
                        break;
                    if (j->fifo_free > 0)
                        break;
                    if (!j->page_fired && j->page_decoding >= j->hw_page)
                        capt_fire_page(j, j->hw_page);
                    if (capt_has_hard_error(j)) {
                        CAPT_LOGW("fifo wait error pg=%u s0=%04x s1=%04x eng=%04x", j->hw_page, j->status0, j->status1,
                                  j->eng_status);
                        j->err = true;
                        break;
                    }
                    capt_sleep_ms(50);
                }
                if (j->fifo_free == 0 && !j->err) {
                    CAPT_LOGW("fifo wait timeout pg=%u s0=%04x s1=%04x", j->hw_page, j->status0, j->status1);
                    j->err = true;
                    break;
                }
            } else if ((j->bands_sent % 16) == 0) {
                capt_wait_ready(j);
            }
        }
        capt_send_flush(j, CAPT_PRINT_DATA, j->comp + off, chunk);
        j->page_bytes_sent += (uint32_t)chunk;
        if (j->fifo_free > 0)
            j->fifo_free--;
        off += chunk;
    }
    j->band_y = 0;
    return j->err ? -1 : 0;
}

static void capt_page_dims(const printer_driver_t *d, int dpi, int paper, int *w_out, int *h_out, int *pw_out,
                           int *ph_out, int *ml_out, int *mt_out)
{
    const printer_paper_t *pp = &d->papers[paper];
    int pw = (int)((long)pp->width * dpi / 2540);
    int ph = (int)((long)pp->height * dpi / 2540);
    int ml = (int)((long)d->margin_left * dpi / 2540);
    int mr = (int)((long)d->margin_right * dpi / 2540);
    int mt = (int)((long)d->margin_top * dpi / 2540);
    int mb = (int)((long)d->margin_bottom * dpi / 2540);
    int w = ((pw - ml - mr) + 31) & ~31;
    int h = ph - mt - mb;
    *w_out = w;
    *h_out = h;
    *pw_out = pw;
    *ph_out = ph;
    *ml_out = ml;
    *mt_out = mt;
}

static void *job_begin(const printer_driver_t *d, const printer_io_t *io, int dpi, int paper)
{
    static const uint8_t magic0[] = {0x00, 0x00, 0x1E, 0x00, 0x00, 0x00, 0x00, 0x00};
    static const uint8_t gpio_2900[12] = {0};
    static const uint8_t gpio_3010[16] = {0x13};
    static const uint8_t lbp3000_init[2] = {0x00, 0x00};
    static const uint8_t lbp6000_init[2] = {0x01, 0x00};

    capt_job_t *j = calloc(1, sizeof(*j));
    if (!j)
        return NULL;
    j->drv = d;
    j->io = *io;
    j->dpi = dpi;
    j->paper = paper;
    j->sides = d->duplex ? io->sides : PRINTER_SIDES_ONE_SIDED;
    capt_page_dims(d, dpi, paper, &j->width, &j->height, &j->paper_w, &j->paper_h, &j->margin_left, &j->margin_top);
    j->line_bytes = (j->width + 7) / 8;
    j->comp_cap = (size_t)CAPT_BAND_LINES * (size_t)j->line_bytes * 2u + 64u;
    j->density = malloc((size_t)j->width);
    j->band = malloc((size_t)CAPT_BAND_LINES * (size_t)j->line_bytes);
    j->comp = malloc(j->comp_cap);
    if (!j->density || !j->band || !j->comp) {
        free(j->density);
        free(j->band);
        free(j->comp);
        free(j);
        return NULL;
    }

    j->job_id = 1;
    uint8_t ident[96] = {0};
    size_t ilen = 0;
    capt_sendrecv(j, CAPT_IDENT, NULL, 0, ident, sizeof(ident), &ilen);
    if (j->io.read && ilen >= 4) {
        capt_log_hex("a1a1", ident, ilen);
        if ((ident[2] | ident[3]) != 0) {
            j->model_id[0] = ident[2];
            j->model_id[1] = ident[3];
            j->has_model_id = true;
        }
    }
    if (j->io.read)
        capt_sleep_ms(200);
    capt_poll_status(j);
    if (j->io.read && variant_of(d) == CAPT_LBP3300) {
        uint8_t ps[160] = {0};
        size_t pslen = 0;
        if (capt_sendrecv(j, CAPT_CHKJOBSTAT, NULL, 0, ps, sizeof(ps), &pslen) == 0 && pslen > 0)
            capt_log_hex("a0a1", ps, pslen);
    }
    capt_sendrecv(j, CAPT_START_0, NULL, 0, NULL, 0, NULL);
    uint8_t jreply[8] = {0};
    size_t jrlen = 0;
    capt_sendrecv(j, CAPT_JOB_BEGIN, magic0, sizeof(magic0), jreply, sizeof(jreply), &jrlen);
    if (j->io.read && jrlen >= 4)
        j->job_id = (uint16_t)(jreply[2] | (jreply[3] << 8));

    capt_variant_t v = variant_of(d);
    CAPT_LOGI("job_begin %s dpi=%d paper=%d sides=%d id=%u s0=%04x s1=%04x", d->id, dpi, paper, j->sides, j->job_id,
              j->status0, j->status1);
    if (v == CAPT_LBP2900) {
        capt_sendrecv(j, CAPT_GPIO, gpio_2900, sizeof(gpio_2900), NULL, 0, NULL);
        capt_wait_ready(j);
        capt_send_job_setup(j, 1, 0);
        capt_wait_ready(j);
    } else if (v == CAPT_LBP3000 || v == CAPT_LBP3300) {
        capt_send_job_setup(j, 1, 0);
        capt_sendrecv(j, CAPT_LBP3000_SETUP_0, lbp3000_init, sizeof(lbp3000_init), NULL, 0, NULL);
        capt_wait_ready(j);
    } else if (v == CAPT_LBP3050) {
        capt_sendrecv(j, CAPT_GPIO, gpio_3010, sizeof(gpio_3010), NULL, 0, NULL);
        capt_wait_ready(j);
        capt_send_job_setup(j, 1, 0);
        capt_wait_ready(j);
    } else {
        capt_sendrecv(j, CAPT_GPIO, gpio_3010, sizeof(gpio_3010), NULL, 0, NULL);
        capt_wait_ready(j);
        capt_sendrecv(j, CAPT_LBP6000_SETUP_0, lbp6000_init, sizeof(lbp6000_init), NULL, 0, NULL);
        capt_wait_ready(j);
        capt_send_job_setup(j, 1, 0);
        capt_wait_ready(j);
    }
    return j;
}

static int page_begin(void *job, int src_w, int src_h, int bytes_pp)
{
    static const uint8_t magic2[16] = {0xEE, 0xDB, 0xEA, 0xAD};
    capt_job_t *j = job;
    if (j->err)
        return -1;
    j->src_w = src_w;
    j->src_h = src_h;
    j->bytes_pp = bytes_pp;
    j->band_y = 0;
    j->total_y = 0;
    j->in_page = true;
    j->page_fired = false;
    j->page_bytes_sent = 0;
    j->page_num++;

    if (j->io.read) {
        capt_poll_status(j);
        if (j->status0 & 0x0030u) {
            capt_sendrecv(j, CAPT_START_1, NULL, 0, NULL, 0, NULL);
            capt_sendrecv(j, CAPT_START_2, NULL, 0, NULL, 0, NULL);
            capt_sendrecv(j, CAPT_START_3, NULL, 0, NULL, 0, NULL);
            capt_wait_ready(j);
            capt_sendrecv(j, CAPT_UPLOAD_2, magic2, sizeof(magic2), NULL, 0, NULL);
            capt_wait_ready(j);
            j->hw_page = 0;
        }
        for (int t = 0; t < 120 && (j->status0 & 0x0004u); t++) {
            capt_sleep_ms(100);
            capt_poll_status(j);
        }
        capt_get_xstatus(j, true);
    }
    j->hw_page++;

    capt_variant_t v = variant_of(j->drv);
    uint8_t sz = j->paper == 1 ? 0x0D : 0x02;   // Letter=0x0D, A4=0x02
    uint8_t b2 = j->has_model_id ? j->model_id[0]
                                 : ((v == CAPT_LBP2900 || v == CAPT_LBP3000 || v == CAPT_LBP3300) ? 0x30 : 0xBA);
    uint8_t b3 = j->has_model_id ? j->model_id[1]
                                 : ((v == CAPT_LBP2900 || v == CAPT_LBP3000 || v == CAPT_LBP3300) ? 0x2A : 0x09);
    uint8_t src = (v == CAPT_LBP3300) ? 0x01 : 0x00;
    if (v == CAPT_LBP3300) {
        uint16_t pw2 = (uint16_t)(j->paper_w >> 1);
        uint16_t ph2 = (uint16_t)(j->paper_h >> 1);
        uint8_t sel_in[8] = {
            sz, 0x01,
            (uint8_t)(pw2 & 0xff), (uint8_t)(pw2 >> 8),
            (uint8_t)(ph2 & 0xff), (uint8_t)(ph2 >> 8),
            0xFF, // Auto (Cassette 1 / Manual Feed)
            (uint8_t)(j->sides != PRINTER_SIDES_ONE_SIDED ? 0x01 : 0x00),
        };
        uint8_t sel_rep[8] = {0};
        size_t sel_len = 0;
        if (capt_sendrecv(j, CAPT_SELECT_INPUT, sel_in, sizeof(sel_in), sel_rep, sizeof(sel_rep), &sel_len) == 0 &&
            j->io.read && sel_len >= 2) {
            CAPT_LOGI("e1a9 select -> status=%02x slot=%02x", sel_rep[0], sel_rep[1]);
            src = sel_rep[1];
        }
    }
    uint8_t dup = 0x00;
    uint8_t p20 = 0x00;
    if (v == CAPT_LBP3300 && j->sides != PRINTER_SIDES_ONE_SIDED)
        dup = (j->page_num % 2) == 1 ? 0x04 : 0x02;
    uint8_t pp[40] = {
        0x00, 0x00, b2, b3, sz, dup, src, 0x00,
        0x1C, 0x1C, 0x1C, 0x1C, 0x00, 0x11, 0x04, 0x00,
        0x01, 0x01, 0x02, 0x00, p20, 0x00,
        (uint8_t)(j->margin_top & 0xff), (uint8_t)(j->margin_top >> 8),
        (uint8_t)(j->margin_left & 0xff), (uint8_t)(j->margin_left >> 8),
        (uint8_t)(j->line_bytes & 0xff), (uint8_t)(j->line_bytes >> 8),
        (uint8_t)(j->height & 0xff), (uint8_t)(j->height >> 8),
        (uint8_t)(j->paper_w & 0xff), (uint8_t)(j->paper_w >> 8),
        (uint8_t)(j->paper_h & 0xff), (uint8_t)(j->paper_h >> 8),
        0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    };
    uint8_t hs[8] = {
        4, 12, 0x01, 0x01, 0, (uint8_t)(int8_t)-5, (uint8_t)(384 & 0xff), (uint8_t)(384 >> 8),
    };
    // Multi-command 0xD0A9 wrapping D0A0, D0A4, D0A1, D0A2
    uint8_t multi[4 + sizeof(pp) + 4 + sizeof(hs) + 4 + 4];
    size_t mp = 0;
    multi[mp++] = 0xA0; multi[mp++] = 0xD0;
    multi[mp++] = (uint8_t)(4 + sizeof(pp)); multi[mp++] = 0;
    memcpy(multi + mp, pp, sizeof(pp));
    mp += sizeof(pp);
    multi[mp++] = 0xA4; multi[mp++] = 0xD0;
    multi[mp++] = (uint8_t)(4 + sizeof(hs)); multi[mp++] = 0;
    memcpy(multi + mp, hs, sizeof(hs));
    mp += sizeof(hs);
    multi[mp++] = 0xA1; multi[mp++] = 0xD0; multi[mp++] = 4; multi[mp++] = 0;
    multi[mp++] = 0xA2; multi[mp++] = 0xD0; multi[mp++] = 4; multi[mp++] = 0;

    capt_log_hex("d0a0", pp, sizeof(pp));
    capt_send_flush(j, CAPT_SET_PARMS, multi, mp);
    CAPT_LOGI("page_begin pg=%u %dx%d (%d B/line) err=%d", j->page_num, j->width, j->height, j->line_bytes, j->err);
    return j->err ? -1 : 0;
}

static int page_line(void *job, int y, const uint8_t *px)
{
    capt_job_t *j = job;
    if (j->err || !j->in_page)
        return -1;
    if (y >= j->height)
        return 0;
    mono_rgb_to_density(px, j->src_w, j->bytes_pp, j->density, j->width);
    mono_dither_1bit(j->density, j->width, y, j->band + (size_t)j->band_y * (size_t)j->line_bytes, j->line_bytes);
    j->band_y++;
    j->total_y++;
    if (j->band_y == CAPT_BAND_LINES && j->total_y < j->height) {
        if (flush_band(j, false) < 0)
            return -1;
    }
    return 0;
}

static int page_end(void *job)
{
    capt_job_t *j = job;
    if (!j->in_page)
        return 0;
    while (!j->err && j->total_y < j->height) {
        memset(j->band + (size_t)j->band_y * (size_t)j->line_bytes, 0, (size_t)j->line_bytes);
        j->band_y++;
        j->total_y++;
        if (j->band_y == CAPT_BAND_LINES && j->total_y < j->height)
            flush_band(j, false);
    }
    flush_band(j, true);
    j->in_page = false;
    if (!j->err)
        capt_send_flush(j, CAPT_PRINT_DATA_END, NULL, 0);

    if (!j->err) {
        if (j->io.read) {
            for (int t = 0; t < 120; t++) {
                if (capt_get_xstatus(j, true) < 0)
                    break;
                if (j->page_received >= j->hw_page && j->page_decoding >= j->hw_page)
                    break;
                if (capt_has_hard_error(j)) {
                    j->err = true;
                    break;
                }
                capt_sleep_ms(100);
            }
        }
        if (!j->err)
            capt_fire_page(j, j->hw_page);
        if (j->io.read && !j->err) {
            for (int t = 0; t < 300; t++) {
                if (capt_get_xstatus(j, true) < 0)
                    break;
                if (j->page_out >= j->hw_page)
                    break;
                if (capt_has_hard_error(j)) {
                    j->err = true;
                    break;
                }
                capt_sleep_ms(100);
            }
            if (!j->err && j->page_out < j->hw_page) {
                CAPT_LOGW("page_end: page %u (hw=%u) did not exit (out=%u s0=%04x s1=%04x s2=%04x eng=%04x)",
                          j->page_num, j->hw_page, j->page_out, j->status0, j->status1, j->status2, j->eng_status);
                j->err = true;
            }
        }
        CAPT_LOGI("page_end pg=%u (hw=%u) sent=%u B dec=%u rx=%u out=%u s0=%04x s1=%04x", j->page_num, j->hw_page,
                  (unsigned)j->page_bytes_sent, j->page_decoding, j->page_received, j->page_out, j->status0,
                  j->status1);
    }
    return j->err ? -1 : 0;
}

static int job_end(void *job)
{
    capt_job_t *j = job;
    if (j->in_page && !j->err)
        page_end(j);
    if (!j->err && variant_of(j->drv) == CAPT_LBP3300 && j->sides != PRINTER_SIDES_ONE_SIDED &&
        (j->page_num % 2) == 1) {
        if (page_begin(j, 1, 1, 1) == 0)
            page_end(j);
    }
    if (!j->err && j->io.read) {
        for (int t = 0; t < 300; t++) {
            if (capt_get_xstatus(j, true) < 0)
                break;
            if (j->page_completed >= j->hw_page)
                break;
            if (capt_has_hard_error(j)) {
                j->err = true;
                break;
            }
            capt_sleep_ms(100);
        }
        if (!j->err && j->hw_page > 0 && j->page_completed < j->hw_page) {
            CAPT_LOGW("job_end: page hw=%u did not complete (comp=%u out=%u s0=%04x s1=%04x eng=%04x)", j->hw_page,
                      j->page_completed, j->page_out, j->status0, j->status1, j->eng_status);
            j->err = true;
        }
    }
    if (!j->io_err) {
        bool saved_err = j->err;
        j->err = false;
        capt_send_job_setup(j, 4, j->hw_page);
        uint8_t jbuf[2] = {(uint8_t)(j->job_id & 0xff), (uint8_t)(j->job_id >> 8)};
        capt_sendrecv(j, CAPT_JOB_END, jbuf, sizeof(jbuf), NULL, 0, NULL);
        if (j->io.flush && j->io.flush(j->io.ctx) < 0)
            j->io_err = true;
        j->err = saved_err || j->io_err;
    }
    int ret = j->err ? -1 : 0;
    CAPT_LOGI("job_end pages=%u ret=%d", j->page_num, ret);
    free(j->density);
    free(j->band);
    free(j->comp);
    free(j);
    return ret;
}

static int test_page(const printer_driver_t *d, const printer_io_t *io, int dpi, const char *label)
{
    capt_job_t *j = job_begin(d, io, dpi, 0);
    if (!j)
        return -1;
    if (j->err || page_begin(j, j->width, j->height, 1) != 0) {
        job_end(j);
        return -1;
    }
    for (int y = 0; y < j->height && !j->err; y++) {
        mono_testpage_line(y, j->width, j->height, dpi, d->make_model, label,
                           j->band + (size_t)j->band_y * (size_t)j->line_bytes, j->line_bytes);
        j->band_y++;
        j->total_y++;
        if (j->band_y == CAPT_BAND_LINES && j->total_y < j->height)
            flush_band(j, false);
        if ((y & 63) == 63 && io->progress)
            io->progress(io->ctx, y + 1, j->height);
    }
    if (!j->err)
        page_end(j);
    return job_end(j);
}

// ---------------------------------------------------------------------------
static const printer_paper_t CAPT_PAPERS[] = {
    {"A4", "iso_a4_210x297mm", 21000, 29700},
    {"Letter", "na_letter_8.5x11in", 21590, 27940},
};

static const capt_model_t M_LBP2900 = {CAPT_LBP2900};
static const capt_model_t M_LBP3000 = {CAPT_LBP3000};
static const capt_model_t M_LBP3050 = {CAPT_LBP3050};
static const capt_model_t M_LBP3300 = {CAPT_LBP3300};
static const capt_model_t M_LBP6000 = {CAPT_LBP6000};

#define CANON_LBP_EX(var, id_, mdl, pid, duplex_, model_ptr)                                                           \
    const printer_driver_t var = {                                                                                     \
        .id = id_,                                                                                                     \
        .make_model = "Canon " mdl,                                                                                    \
        .usb_mfg = "Canon",                                                                                            \
        .usb_mdl = mdl,                                                                                                \
        .usb_vid = 0x04a9,                                                                                             \
        .usb_pid = pid,                                                                                                \
        .color = false,                                                                                                \
        .scanner = false,                                                                                              \
        .duplex = duplex_,                                                                                             \
        .resolutions = {600},                                                                                          \
        .nresolutions = 1,                                                                                             \
        .papers = CAPT_PAPERS,                                                                                         \
        .npapers = 2,                                                                                                  \
        .margin_left = 500,                                                                                            \
        .margin_right = 500,                                                                                           \
        .margin_top = 500,                                                                                             \
        .margin_bottom = 500,                                                                                          \
        .model = model_ptr,                                                                                            \
        .job_begin = job_begin,                                                                                        \
        .page_begin = page_begin,                                                                                      \
        .page_line = page_line,                                                                                        \
        .page_end = page_end,                                                                                          \
        .job_end = job_end,                                                                                            \
        .test_page = test_page,                                                                                        \
        .status = capt_status,                                                                                         \
        .poll_status = capt_idle_poll_status,                                                                          \
    }

#define CANON_LBP(var, id_, mdl, pid, model_ptr) CANON_LBP_EX(var, id_, mdl, pid, false, model_ptr)

CANON_LBP(canon_lbp2900_driver, "canon-lbp2900", "LBP2900", 0x2676, &M_LBP2900);
CANON_LBP(canon_lbp3000_driver, "canon-lbp3000", "LBP3000", 0x266a, &M_LBP3000);
CANON_LBP(canon_lbp3050_driver, "canon-lbp3050", "LBP3050", 0x26da, &M_LBP3050);
CANON_LBP_EX(canon_lbp3300_driver, "canon-lbp3300", "LBP3300", 0x267e, true, &M_LBP3300);
CANON_LBP(canon_lbp6018_driver, "canon-lbp6018", "LBP6018", 0x2774, &M_LBP6000);
CANON_LBP(canon_lbp6030_driver, "canon-lbp6030", "LBP6030/6040/6018L", 0x2794, &M_LBP6000);
CANON_LBP(canon_lbp6040_driver, "canon-lbp6040", "LBP6040", 0x2795, &M_LBP6000);
