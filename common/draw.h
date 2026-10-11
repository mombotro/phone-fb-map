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
    int theme;                  // 1 = the palmtop look: every colour is mapped to a grey-green LCD palette. 0 = colours as given.
} Surf;

// The palmtop look. The programs are drawn in a dark palette (dark background, light text). With the theme on, a dark colour becomes
// the pale LCD colour, a light colour becomes the dark "ink", and the colours between become six shades. Text then picks ink or
// paper by what is under it, so it always reads. The title-bar colours below are always ink (a black Palm title bar).
#define TH_PAPER 0xC6CFAE
#define TH_INK   0x1F2A1B
static uint32_t draw_theme_color(uint32_t rgb) {
    if (rgb == 0x26303E || rgb == 0x1B2330) return TH_INK;                    // the bars and panels of the programs
    int r = (rgb >> 16) & 255, g = (rgb >> 8) & 255, b = rgb & 255;
    int l = (r * 30 + g * 59 + b * 11) / 100;
    int t = (l - 20) * 255 / 215; if (t < 0) t = 0; if (t > 255) t = 255;
    t = (t * 5 + 127) / 255;                                                  // 0..5
    int pr = (TH_PAPER >> 16) & 255, pg = (TH_PAPER >> 8) & 255, pb = TH_PAPER & 255;
    int ir = (TH_INK >> 16) & 255, ig = (TH_INK >> 8) & 255, ib = TH_INK & 255;
    return (uint32_t)(pr + (ir - pr) * t / 5) << 16 | (uint32_t)(pg + (ig - pg) * t / 5) << 8 | (uint32_t)(pb + (ib - pb) * t / 5);
}
static int draw_lum(uint32_t rgb) { return (((rgb >> 16) & 255) * 30 + ((rgb >> 8) & 255) * 59 + (rgb & 255) * 11) / 100; }

static uint32_t draw_pack(const Surf *s, uint32_t rgb) {
    if (s->theme) rgb = draw_theme_color(rgb);
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
    uint32_t c;
    if (s->theme && x >= 0 && y >= 0 && x < s->w && y < s->h) {               // look at what is under the first letter, and keep the contrast
        uint32_t d = s->px[(size_t)y * s->stride + x];
        uint32_t dest = (((d >> s->rs) & 255) << 16) | (((d >> s->gs) & 255) << 8) | ((d >> s->bs) & 255);
        uint32_t want = draw_theme_color(rgb);
        int dl = draw_lum(dest), wl = draw_lum(want);
        if ((dl > wl ? dl - wl : wl - dl) < 90) want = dl > 128 ? TH_INK : TH_PAPER;
        c = (((want >> 16) & 0xFF) << s->rs) | (((want >> 8) & 0xFF) << s->gs) | ((want & 0xFF) << s->bs) | s->alpha;
    } else c = draw_pack(s, rgb);
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
