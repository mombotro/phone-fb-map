// console.h - text console on a pixel buffer. 8x16 font, persistent cell grid, scrolling.
// Redraw the whole grid into each back buffer with con_render() every frame.
#ifndef CONSOLE_H
#define CONSOLE_H

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "font8x16.h"

#define CON_CW 8
#define CON_CH 16

typedef struct {
    int cols, rows, cx, cy;
    int scale;               // integer pixel scale of the 8x16 font
    char *ch;                // rows*cols characters
    uint32_t *fg;            // rows*cols colors, 0xRRGGBB
    uint32_t cur_fg, bg;     // 0xRRGGBB
    int rs, gs, bs;          // channel shifts in the target pixel format
    uint32_t alpha;          // OR'd into every pixel (0 if the format ignores alpha)
} Con;

static int con_init(Con *c, int w, int h, int scale) {
    memset(c, 0, sizeof *c);
    c->scale = scale < 1 ? 1 : scale;
    c->cols = w / (CON_CW * c->scale);
    c->rows = h / (CON_CH * c->scale);
    c->ch = malloc((size_t)c->cols * c->rows);
    c->fg = malloc((size_t)c->cols * c->rows * sizeof(uint32_t));
    if (!c->ch || !c->fg) return -1;
    c->cur_fg = 0xCCCCCC;
    c->bg = 0x000000;
    memset(c->ch, ' ', (size_t)c->cols * c->rows);
    for (int i = 0; i < c->cols * c->rows; i++) c->fg[i] = c->cur_fg;
    return 0;
}

static void con_color(Con *c, uint32_t rgb) { c->cur_fg = rgb; }

static void con__scroll(Con *c) {
    size_t row = (size_t)c->cols;
    memmove(c->ch, c->ch + row, row * (c->rows - 1));
    memmove(c->fg, c->fg + row, row * (c->rows - 1) * sizeof(uint32_t));
    memset(c->ch + row * (c->rows - 1), ' ', row);
    for (int x = 0; x < c->cols; x++) c->fg[row * (c->rows - 1) + x] = c->cur_fg;
    c->cy = c->rows - 1;
}

static void con__newline(Con *c) {
    c->cx = 0;
    if (++c->cy >= c->rows) con__scroll(c);
}

static void con_putc(Con *c, int ch) {
    if (ch == '\n') { con__newline(c); return; }
    if (ch == '\r') { c->cx = 0; return; }
    if (ch == '\t') { c->cx = (c->cx + 8) & ~7; if (c->cx >= c->cols) con__newline(c); return; }
    if (ch < 32 || ch > 126) ch = '?';
    if (c->cx >= c->cols) con__newline(c);
    c->ch[c->cy * c->cols + c->cx] = (char)ch;
    c->fg[c->cy * c->cols + c->cx] = c->cur_fg;
    c->cx++;
}

static void con_puts(Con *c, const char *s) { while (*s) con_putc(c, (unsigned char)*s++); }

static void con_printf(Con *c, const char *fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    con_puts(c, buf);
}

static uint32_t con__pack(const Con *c, uint32_t rgb) {
    return (((rgb >> 16) & 0xFF) << c->rs) | (((rgb >> 8) & 0xFF) << c->gs) |
           ((rgb & 0xFF) << c->bs) | c->alpha;
}

// Draw the whole grid (and bg margins) into px. stride_px is in pixels, not bytes.
static void con_render(const Con *c, uint32_t *px, int stride_px, int w, int h, int show_cursor) {
    uint32_t bgp = con__pack(c, c->bg);
    int S = c->scale, cell_w = CON_CW * S, cell_h = CON_CH * S;
    for (int r = 0; r < c->rows; r++) {
        for (int gy = 0; gy < CON_CH; gy++) {
            uint32_t *line = px + (size_t)(r * cell_h + gy * S) * stride_px;
            for (int col = 0; col < c->cols; col++) {
                int i = r * c->cols + col;
                uint32_t fgp = con__pack(c, c->fg[i]);
                uint32_t a = fgp, b = bgp;
                if (show_cursor && r == c->cy && col == c->cx) { a = bgp; b = fgp; }  // inverse block
                uint8_t bits = font8x16[c->ch[i] - 32][gy];
                uint32_t *d = line + col * cell_w;
                for (int x = 0; x < CON_CW; x++) {
                    uint32_t v = (bits & (0x80 >> x)) ? a : b;
                    for (int k = 0; k < S; k++) d[x * S + k] = v;
                }
            }
            for (int x = c->cols * cell_w; x < w; x++) line[x] = bgp;   // right margin
            for (int k = 1; k < S; k++)                                  // repeat the row S times
                memcpy(line + (size_t)k * stride_px, line, (size_t)w * sizeof(uint32_t));
        }
    }
    for (int y = c->rows * cell_h; y < h; y++) {                         // bottom margin
        uint32_t *line = px + (size_t)y * stride_px;
        for (int x = 0; x < w; x++) line[x] = bgp;
    }
}

#endif
