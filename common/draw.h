// draw.h - minimal 2D helpers on a 32-bit pixel buffer. Colors are 0xRRGGBB.
#ifndef DRAW_H
#define DRAW_H

#include <stdint.h>
#include <string.h>
#include "font8x16.h"
#include "theme.h"

typedef struct {
    uint32_t *px;
    int stride;                 // pixels per row
    int w, h;
    int rs, gs, bs;
    uint32_t alpha;
    int theme;                  // 0 = colours as given. 1 and up: the number of a colour theme (theme.h): every colour is mapped to its three colours.
} Surf;

// The colour themes (theme.h). With a theme on, a dark colour becomes the PAPER colour, a light colour becomes the INK, and the colours between
// become six shades on the way PAPER, MID, INK. Text then picks ink or paper by what is under it, so it always reads. The title-bar colours below
// are always ink (like a black Palm title bar).
static uint32_t draw_theme_color(int th, uint32_t rgb) {
    if (th <= 0 || th >= THEME_N) return rgb;
    const Theme *T = &THEMES[th];
    if (rgb == 0x26303E || rgb == 0x1B2330) return T->ink;                     // the bars and panels of the programs
    int r = (rgb >> 16) & 255, g = (rgb >> 8) & 255, b = rgb & 255;
    int l = (r * 30 + g * 59 + b * 11) / 100;
    int t = (l - 20) * 255 / 215; if (t < 0) t = 0; if (t > 255) t = 255;
    t = (t * 5 + 127) / 255;                                                  // 0..5
    uint32_t a0, a1; int num, den;                                            // the first half goes from paper to mid, the second from mid to ink
    if (t * 2 <= 5) { a0 = T->paper; a1 = T->mid; num = t * 2; den = 5; }
    else            { a0 = T->mid;   a1 = T->ink; num = t * 2 - 5; den = 5; }
    int c[3];
    for (int k = 0; k < 3; k++) { int sh = 16 - 8 * k, x0 = (a0 >> sh) & 255, x1 = (a1 >> sh) & 255; c[k] = x0 + (x1 - x0) * num / den; }
    return (uint32_t)c[0] << 16 | (uint32_t)c[1] << 8 | (uint32_t)c[2];
}
static int draw_lum(uint32_t rgb) { return (((rgb >> 16) & 255) * 30 + ((rgb >> 8) & 255) * 59 + (rgb & 255) * 11) / 100; }
// The colour of text in a theme: the mapped colour, but if it is too close to what is under the text, whichever of paper and ink is farther from it.
static uint32_t draw_theme_text(int th, uint32_t dest, uint32_t rgb) {
    uint32_t want = draw_theme_color(th, rgb);
    int dl = draw_lum(dest), wl = draw_lum(want);
    if ((dl > wl ? dl - wl : wl - dl) < 90) {
        int pl = draw_lum(THEMES[th].paper), il = draw_lum(THEMES[th].ink);
        want = (dl > pl ? dl - pl : pl - dl) > (dl > il ? dl - il : il - dl) ? THEMES[th].paper : THEMES[th].ink;
    }
    return want;
}

static uint32_t draw_pack(const Surf *s, uint32_t rgb) {
    if (s->theme) rgb = draw_theme_color(s->theme, rgb);
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

#ifdef UI_FONT
// ttf.h: draws the text with the reading font chosen in the picker settings. Returns 0 when no font is chosen.
static int ui_font_draw(Surf *s, int x, int y, int scale, const char *str, uint32_t rgb);
#endif

// Text with the chunky 8x16 bitmap font, integer scale, transparent background. Used where a fixed cell matters (editors).
static void draw_text_plain(Surf *s, int x, int y, int scale, const char *str, uint32_t rgb) {
    uint32_t c;
    if (s->theme && x >= 0 && y >= 0 && x < s->w && y < s->h) {               // look at what is under the first letter, and keep the contrast
        uint32_t d = s->px[(size_t)y * s->stride + x];
        uint32_t dest = (((d >> s->rs) & 255) << 16) | (((d >> s->gs) & 255) << 8) | ((d >> s->bs) & 255);
        uint32_t want = draw_theme_text(s->theme, dest, rgb);
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

// Text at a cell size of 8*scale by 16*scale. With a reading font chosen (picker settings), the same text is drawn in that font,
// sized so its average letter is as wide as a chunky cell. Without one, it is the chunky bitmap font.
static void draw_text(Surf *s, int x, int y, int scale, const char *str, uint32_t rgb) {
#ifdef UI_FONT
    if (ui_font_draw(s, x, y, scale, str, rgb)) return;
#endif
    draw_text_plain(s, x, y, scale, str, rgb);
}

#ifdef UI_FONT
#include "ttf.h"
#endif

#endif
