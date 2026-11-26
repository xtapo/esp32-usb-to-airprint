// Portable monochrome (1-bit) rasterization and test page generator.
// Used by laser printer drivers (HP LaserJet, Canon LBP, Brother).
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Converts one scanline of gray (bytes_pp == 1) or RGB (bytes_pp == 3) pixels
// into 0..255 ink density (0 = white, 255 = black), padded with white if
// src_w < dst_w or clipped if src_w > dst_w. px == NULL produces white.
void mono_rgb_to_density(const uint8_t *px, int src_w, int bytes_pp, uint8_t *density, int dst_w);

// Ordered 8x8 Bayer dithering from 0..255 ink density into 1-bit MSB-first
// packed pixels (1 = black dot, 0 = white). out_bytes >= (width + 7) / 8.
void mono_dither_1bit(const uint8_t *density, int width, int y, uint8_t *out, int out_bytes);

// Renders one 1-bit MSB-first scanline (1 = black) of a clean monochrome test
// page with border, inch ruler ticks, halftone ramp, and device/DPI text.
void mono_testpage_line(int y, int width, int height, int dpi, const char *title, const char *label, uint8_t *out,
                        int out_bytes);

#ifdef __cplusplus
}
#endif
