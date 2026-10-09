// draw.h - minimal 2D helpers on a 32-bit pixel buffer. Colors are 0xRRGGBB.
#ifndef DRAW_H
#define DRAW_H

#include <stdint.h>
#include <string.h>
#include "font8x16.h"

typedef struct {
    uint32_t *px;
    int stride;                 // pixels per row
    int w, h;
    int rs, gs, bs;
    uint32_t alpha;
} Surf;

static uint32_t draw_pack(const Surf *s, uint32_t rgb) {
    return (((rgb >> 16) & 0xFF) << s->rs) | (((rgb >> 8) & 0xFF) << s->gs) |
           ((rgb & 0xFF) << s->bs) | s->alpha;
}

static void draw_rect(Surf *s, int x0, int y0, int w, int h, uint32_t rgb) {
    uint32_t c = draw_pack(s, rgb);
    for (int y = y0; y < y0 + h; y++) {
        if (y < 0 || y >= s->h) continue;
        for (int x = x0; x < x0 + w; x++)
            if (x >= 0 && x < s->w) s->px[(size_t)y * s->stride + x] = c;
    }
}

static void draw_disk(Surf *s, int cx, int cy, int r, uint32_t rgb) {
    uint32_t c = draw_pack(s, rgb);
    for (int y = -r; y <= r; y++) {
        int yy = cy + y;
        if (yy < 0 || yy >= s->h) continue;
        for (int x = -r; x <= r; x++) {
            int xx = cx + x;
            if (xx >= 0 && xx < s->w && x * x + y * y <= r * r) s->px[(size_t)yy * s->stride + xx] = c;
        }
    }
}

static void draw_line(Surf *s, int x0, int y0, int x1, int y1, int r, uint32_t rgb) {
    int dx = x1 - x0, dy = y1 - y0;
    int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    int steps = (adx > ady ? adx : ady) / 2 + 1;
    for (int i = 0; i <= steps; i++) draw_disk(s, x0 + dx * i / steps, y0 + dy * i / steps, r, rgb);
}

// Text with the 8x16 font, integer scale, transparent background.
static void draw_text(Surf *s, int x, int y, int scale, const char *str, uint32_t rgb) {
    uint32_t c = draw_pack(s, rgb);
    for (; *str; str++, x += 8 * scale) {
        int ch = (unsigned char)*str;
        if (ch < 32 || ch > 126) ch = '?';
        for (int gy = 0; gy < 16; gy++) {
            uint8_t bits = font8x16[ch - 32][gy];
            for (int gx = 0; gx < 8; gx++) {
                if (!(bits & (0x80 >> gx))) continue;
                for (int ky = 0; ky < scale; ky++) {
                    int yy = y + gy * scale + ky;
                    if (yy < 0 || yy >= s->h) continue;
                    for (int kx = 0; kx < scale; kx++) {
                        int xx = x + gx * scale + kx;
                        if (xx >= 0 && xx < s->w) s->px[(size_t)yy * s->stride + xx] = c;
                    }
                }
            }
        }
    }
}

#endif
