// term.h - tiny terminal emulator front end for console.h. Handles \r \n \b \t and the common
// ANSI sequences a shell uses for line editing (CSI A B C D G H J K P X); colors are ignored.
#ifndef TERM_H
#define TERM_H

#include "console.h"

typedef struct { int state, np, have; int p[4]; int priv; } Term;   // state: 0 text 1 ESC 2 CSI 3 OSC 4 skip1

static void term__clear_range(Con *c, int row, int from, int to) {            // [from, to)
    for (int x = from; x < to && x < c->cols; x++) {
        c->ch[row * c->cols + x] = ' ';
        c->fg[row * c->cols + x] = c->cur_fg;
    }
}

static int term__arg(const Term *t, int i, int def) { return (i < t->np && t->p[i] > 0) ? t->p[i] : def; }

static void term__csi(Term *t, Con *c, int final) {
    if (t->priv) return;
    int n = term__arg(t, 0, 1);
    switch (final) {
        case 'A': c->cy -= n; if (c->cy < 0) c->cy = 0; break;
        case 'B': c->cy += n; if (c->cy >= c->rows) c->cy = c->rows - 1; break;
        case 'C': c->cx += n; if (c->cx >= c->cols) c->cx = c->cols - 1; break;
        case 'D': c->cx -= n; if (c->cx < 0) c->cx = 0; break;
        case 'G': c->cx = n - 1; if (c->cx < 0) c->cx = 0; if (c->cx >= c->cols) c->cx = c->cols - 1; break;
        case 'H': case 'f': {
            int r = term__arg(t, 0, 1) - 1, col = term__arg(t, 1, 1) - 1;
            c->cy = r < 0 ? 0 : r >= c->rows ? c->rows - 1 : r;
            c->cx = col < 0 ? 0 : col >= c->cols ? c->cols - 1 : col;
            break;
        }
        case 'K': {
            int m = (t->np > 0) ? t->p[0] : 0;
            if (m == 0) term__clear_range(c, c->cy, c->cx, c->cols);
            else if (m == 1) term__clear_range(c, c->cy, 0, c->cx + 1);
            else term__clear_range(c, c->cy, 0, c->cols);
            break;
        }
        case 'J': {
            int m = (t->np > 0) ? t->p[0] : 0;
            if (m == 0) { term__clear_range(c, c->cy, c->cx, c->cols); for (int r = c->cy + 1; r < c->rows; r++) term__clear_range(c, r, 0, c->cols); }
            else if (m >= 2) { for (int r = 0; r < c->rows; r++) term__clear_range(c, r, 0, c->cols); c->cx = c->cy = 0; }
            break;
        }
        case 'P': {                                                          // delete n chars at cursor
            int row = c->cy * c->cols;
            for (int x = c->cx; x < c->cols; x++) {
                int src = x + n;
                c->ch[row + x] = src < c->cols ? c->ch[row + src] : ' ';
                c->fg[row + x] = src < c->cols ? c->fg[row + src] : c->cur_fg;
            }
            break;
        }
        case 'X': term__clear_range(c, c->cy, c->cx, c->cx + n); break;
        default: break;                                                      // m, h, l, r, ...: ignore
    }
}

static void term_feed(Term *t, Con *c, const unsigned char *buf, int len) {
    for (int i = 0; i < len; i++) {
        int b = buf[i];
        switch (t->state) {
            case 1:                                                          // after ESC
                if (b == '[') { t->state = 2; t->np = 0; t->have = 0; t->priv = 0; memset(t->p, 0, sizeof t->p); }
                else if (b == ']') t->state = 3;
                else if (b == '(' || b == ')' || b == '#') t->state = 4;
                else t->state = 0;
                break;
            case 2:                                                          // CSI parameters
                if (b >= '0' && b <= '9') {
                    if (t->np == 0) t->np = 1;
                    if (t->np <= 4) t->p[t->np - 1] = t->p[t->np - 1] * 10 + (b - '0');
                    t->have = 1;
                } else if (b == ';') { if (t->np == 0) t->np = 1; if (t->np < 4) t->np++; }
                else if (b == '?' || b == '>' || b == '=') t->priv = 1;
                else if (b >= 0x40 && b <= 0x7E) { term__csi(t, c, b); t->state = 0; }
                else if (b >= 0x20 && b <= 0x2F) { /* intermediate */ }
                else t->state = 0;
                break;
            case 3:                                                          // OSC: until BEL or ESC
                if (b == 7) t->state = 0; else if (b == 27) t->state = 4;
                break;
            case 4: t->state = 0; break;
            default:
                if (b == 27) t->state = 1;
                else if (b == '\b') { if (c->cx > 0) c->cx--; }
                else if (b == '\r' || b == '\n' || b == '\t') con_putc(c, b);
                else if (b >= 32 && b < 127) con_putc(c, b);
                else if (b >= 0x80) con_putc(c, '?');
                break;
        }
    }
}

#endif
