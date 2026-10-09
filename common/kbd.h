// kbd.h - on-screen QWERTY keyboard for touch + framebuffer. Layers: 0 lower, 1 upper, 2 symbols.
// Keys fire on touch-down. Shift is one-shot; Ctrl is one-shot and turns the next letter into ^letter.
#ifndef KBD_H
#define KBD_H

#include <stdio.h>
#include <string.h>
#include "draw.h"

enum { K_SHIFT = 0x100, K_SYM, K_BKSP, K_ENTER, K_CTRL, K_TAB, K_SPACE };

typedef struct { int x, y, w, h; char label[8]; int code; } KbKey;
typedef struct { const char *label; int code; int wt; } KbSpec;

typedef struct {
    int x0, y0, w, h;
    KbKey keys[3][48];
    int n[3];
    char lab[128][2];             // one-character label storage
    int nlab;
    int layer, ctrl;
    int down[16];                    // key index + 1 held by each touch id, 0 = none
    int dirty;
} Kbd;

static const char *kbd__lab(Kbd *k, char c) {
    k->lab[k->nlab][0] = c; k->lab[k->nlab][1] = 0;
    return k->lab[k->nlab++];
}

static void kbd__row(Kbd *k, int layer, int row, const KbSpec *sp, int n) {
    int total = 0, acc = 0, rowh = k->h / 5;
    for (int i = 0; i < n; i++) total += sp[i].wt;
    for (int i = 0; i < n; i++) {
        int xa = k->x0 + k->w * acc / total, xb = k->x0 + k->w * (acc + sp[i].wt) / total;
        acc += sp[i].wt;
        if (!sp[i].code) continue;                           // spacer
        KbKey *kk = &k->keys[layer][k->n[layer]++];
        kk->x = xa + 2; kk->y = k->y0 + row * rowh + 2; kk->w = xb - xa - 4; kk->h = rowh - 4;
        snprintf(kk->label, sizeof kk->label, "%s", sp[i].label);
        kk->code = sp[i].code;
    }
}

// lead/trail: optional special keys (code 0 = none); chars: one key per char; pad: half-key spacers.
static void kbd__chars(Kbd *k, int layer, int row, const KbSpec *lead, const char *chars,
                       const KbSpec *trail, int pad) {
    KbSpec sp[16]; int n = 0;
    if (lead) sp[n++] = *lead;
    if (pad) sp[n++] = (KbSpec){ "", 0, 5 };
    for (const char *c = chars; *c; c++) sp[n++] = (KbSpec){ kbd__lab(k, *c), *c, 10 };
    if (pad) sp[n++] = (KbSpec){ "", 0, 5 };
    if (trail) sp[n++] = *trail;
    kbd__row(k, layer, row, sp, n);
}

static void kbd_init(Kbd *k, int x0, int y0, int w, int h) {
    memset(k, 0, sizeof *k);
    k->x0 = x0; k->y0 = y0; k->w = w; k->h = h;
    static const char *r0[3] = { "1234567890", "1234567890", "!@#$%^&*()" };
    static const char *r1[3] = { "qwertyuiop", "QWERTYUIOP", "~`|\\{}[]<>" };
    static const char *r2[3] = { "asdfghjkl", "ASDFGHJKL", "-_=+:;\"'?" };
    static const char *r3[3] = { "zxcvbnm", "ZXCVBNM", ",./_-=+" };
    KbSpec shift = { "SHIFT", K_SHIFT, 15 }, abc = { "ABC", K_SYM, 15 }, bksp = { "<-", K_BKSP, 15 };
    for (int L = 0; L < 3; L++) {
        kbd__chars(k, L, 0, NULL, r0[L], NULL, 0);
        kbd__chars(k, L, 1, NULL, r1[L], NULL, 0);
        kbd__chars(k, L, 2, NULL, r2[L], NULL, 1);
        kbd__chars(k, L, 3, L == 2 ? &abc : &shift, r3[L], &bksp, 0);
        KbSpec sym = { L == 2 ? "ABC" : "?123", K_SYM, 15 };
        KbSpec bottom[] = {
            sym, { "CTL", K_CTRL, 12 }, { "TAB", K_TAB, 12 }, { "", K_SPACE, 34 },
            { ".", '.', 10 }, { "/", '/', 10 }, { "ENT", K_ENTER, 18 },
        };
        kbd__row(k, L, 4, bottom, sizeof bottom / sizeof bottom[0]);
    }
    k->dirty = 1;
}

// Touch-down at (x,y) for touch id. Returns the byte to send to the shell, or -1 for none.
static int kbd_press(Kbd *k, int id, int x, int y) {
    id &= 15;
    if (y < k->y0 || y >= k->y0 + k->h) return -1;
    for (int i = 0; i < k->n[k->layer]; i++) {
        KbKey *kk = &k->keys[k->layer][i];
        if (x < kk->x || x >= kk->x + kk->w || y < kk->y || y >= kk->y + kk->h) continue;
        k->down[id] = i + 1;
        k->dirty = 1;
        int c = kk->code;
        switch (c) {
            case K_SHIFT: k->layer = k->layer == 1 ? 0 : 1; return -1;
            case K_SYM:   k->layer = k->layer == 2 ? 0 : 2; return -1;
            case K_CTRL:  k->ctrl ^= 1; return -1;
            case K_BKSP:  return 0x7f;
            case K_ENTER: return '\r';
            case K_TAB:   return '\t';
            case K_SPACE: return ' ';
        }
        if (k->ctrl) {
            if (c >= 'a' && c <= 'z') c = c - 'a' + 1;
            else if (c >= 'A' && c <= 'Z') c = c - 'A' + 1;
            else c &= 0x1f;
            k->ctrl = 0;
        }
        if (k->layer == 1) k->layer = 0;                     // one-shot shift
        return c;
    }
    return -1;
}

static void kbd_release(Kbd *k, int id) {
    id &= 15;
    if (k->down[id]) { k->down[id] = 0; k->dirty = 1; }
}

// Code of the key currently held by this touch id (0 if none) - used for backspace auto-repeat.
static int kbd_held_code(const Kbd *k, int id) {
    int d = k->down[id & 15];
    return d ? k->keys[k->layer][d - 1].code : 0;
}

static void kbd_render(const Kbd *k, Surf *s) {
    draw_rect(s, k->x0, k->y0, k->w, k->h, 0x12161C);
    for (int i = 0; i < k->n[k->layer]; i++) {
        const KbKey *kk = &k->keys[k->layer][i];
        int held = 0;
        for (int id = 0; id < 16; id++) if (k->down[id] == i + 1) held = 1;
        uint32_t col = kk->code >= 0x100 ? 0x2A3850 : 0x38404E;
        if ((kk->code == K_SHIFT && k->layer == 1) || (kk->code == K_CTRL && k->ctrl)) col = 0x8A6A1A;
        if (held) col = 0x5A80B8;
        draw_rect(s, kk->x, kk->y, kk->w, kk->h, col);
        int len = (int)strlen(kk->label);
        int sc = len * 16 <= kk->w - 6 ? 2 : 1;
        draw_text(s, kk->x + (kk->w - len * 8 * sc) / 2, kk->y + (kk->h - 16 * sc) / 2, sc, kk->label, 0xFFFFFF);
    }
}

#endif
