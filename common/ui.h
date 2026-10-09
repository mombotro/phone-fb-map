// ui.h - a small touch UI toolkit for the framebuffer apps. Header only. Needs plat.h, draw.h and kbd.h first.
//
// Call ui_poll() once per frame with the touches. It tracks the first finger as a pointer and reports taps:
// a tap is a short touch that did not move. Widgets read that pointer state, draw themselves, and return events.
//   ui_button()    a labelled button, returns 1 on a tap
//   ui_scroll()    drag to scroll a region
//   UiEdit         a multi-line text editor with word wrap, a cursor, and tick-box lines ("[ ] " and "[x] ")
//   ui_keyboard()  turns touches on the on-screen keyboard into key codes, with backspace repeat
#ifndef FBUI_H
#define FBUI_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include "kbd.h"

static double ui_now(void) { struct timeval tv; gettimeofday(&tv, NULL); return tv.tv_sec + tv.tv_usec / 1e6; }

typedef struct {
    Surf *s;
    int W, H;
    int down, x, y;                 // the first finger: touching now, and where
    int start_x, start_y;           // where this touch began
    int moved;                      // it moved more than the slop since it began
    double t_start;
    int tapped, tap_x, tap_y;       // true for one frame after a short, still touch ends
    int drag_dy;                    // vertical movement this frame while dragging
    int changed;                    // the pointer did something this frame: redraw
    int prev_down, last_y;
    int kb_prev[16];                // keyboard: which touch ids were down last frame
    double kb_t[16], kb_rep[16];
} Ui;

#define UI_SLOP 16

static void ui_poll(Ui *u, const PlatTouch *t, int n) {
    u->tapped = 0; u->drag_dy = 0; u->changed = 0;
    int d = n > 0;
    if (d) { u->x = t[0].x; u->y = t[0].y; }
    if (d && !u->prev_down) {                                     // touch began
        u->start_x = u->x; u->start_y = u->y; u->moved = 0; u->t_start = ui_now(); u->last_y = u->y; u->changed = 1;
    }
    if (d && u->prev_down) {
        if (abs(u->x - u->start_x) > UI_SLOP || abs(u->y - u->start_y) > UI_SLOP) u->moved = 1;
        if (u->moved) { u->drag_dy = u->y - u->last_y; if (u->drag_dy) u->changed = 1; }
        u->last_y = u->y;
    }
    if (!d && u->prev_down) {                                     // touch ended
        if (!u->moved && ui_now() - u->t_start < 0.8) { u->tapped = 1; u->tap_x = u->start_x; u->tap_y = u->start_y; }
        u->changed = 1;
    }
    u->down = d; u->prev_down = d;
}

static int ui_inside(int px, int py, int x, int y, int w, int h) { return px >= x && px < x + w && py >= y && py < y + h; }
static int ui_tapped_in(const Ui *u, int x, int y, int w, int h) { return u->tapped && ui_inside(u->tap_x, u->tap_y, x, y, w, h); }

static void ui_label(Ui *u, int x, int y, int scale, const char *text, uint32_t rgb) { draw_text(u->s, x, y, scale, text, rgb); }

// Truncate text to fit `cols` characters (adds "~"). Writes into dst.
static void ui_fit(char *dst, size_t n, const char *src, int cols) {
    snprintf(dst, n, "%s", src);
    if (cols > 0 && (int)strlen(dst) > cols) { dst[cols - 1] = '~'; dst[cols] = 0; }
}

// A button. Pressed look while the finger is down on it. Returns 1 when it was tapped.
static int ui_button(Ui *u, int x, int y, int w, int h, const char *label, uint32_t color) {
    int pressed = u->down && ui_inside(u->start_x, u->start_y, x, y, w, h) && ui_inside(u->x, u->y, x, y, w, h);
    draw_rect(u->s, x, y, w, h, pressed ? 0x5A80B8 : color);
    int len = (int)strlen(label), sc = len * 16 <= w - 8 ? 2 : 1;
    draw_text(u->s, x + (w - len * 8 * sc) / 2, y + (h - 16 * sc) / 2, sc, label, 0xFFFFFF);
    return ui_tapped_in(u, x, y, w, h);
}

// ---------------------------------------------------------------- scrolling

typedef struct { int pos; } UiScroll;

// Drag inside the region (x,y,w,h) to scroll content of height content_h. Keeps pos in range.
static void ui_scroll(Ui *u, UiScroll *sc, int x, int y, int w, int h, int content_h) {
    if (u->down && u->moved && ui_inside(u->start_x, u->start_y, x, y, w, h)) sc->pos -= u->drag_dy;
    int max = content_h - h; if (max < 0) max = 0;
    if (sc->pos > max) sc->pos = max;
    if (sc->pos < 0) sc->pos = 0;
}

// ---------------------------------------------------------------- text editor

typedef struct {
    char *buf; int len, cap;
    int cur;                        // cursor: index into buf
    UiScroll scroll;
    int reveal;                     // scroll so the cursor is visible on the next draw
    int edited;                     // changed since the last save
} UiEdit;

typedef struct { int start, len, lstart, box; } VLine;   // box: 0 none, 1 open, 2 done (on the first visual line only)
#define UI_MAXV 4000

static void ui_edit_init(UiEdit *e, int cap) { memset(e, 0, sizeof *e); e->cap = cap; e->buf = calloc(1, cap + 1); }
static void ui_edit_set(UiEdit *e, const char *text) {
    int n = (int)strlen(text); if (n > e->cap) n = e->cap;
    memcpy(e->buf, text, n); e->buf[n] = 0; e->len = n; e->cur = n; e->scroll.pos = 0; e->reveal = 1; e->edited = 0;
}

static int ui_is_box(const char *b, int ls, int le) {
    return le - ls >= 4 && b[ls] == '[' && (b[ls + 1] == ' ' || b[ls + 1] == 'x' || b[ls + 1] == 'X') && b[ls + 2] == ']' && b[ls + 3] == ' ';
}

// Break the text into visual lines of at most `cols` characters, wrapping at spaces where possible.
static int ui_edit_layout(const UiEdit *e, int cols, VLine *vl, int max) {
    int n = 0, pos = 0;
    while (pos <= e->len && n < max - 1) {
        int ls = pos, le = ls; while (le < e->len && e->buf[le] != '\n') le++;
        int box = ui_is_box(e->buf, ls, le) ? (e->buf[ls + 1] == ' ' ? 1 : 2) : 0;
        int first = 1, p = ls;
        while (le - p > cols && n < max - 1) {
            int b = -1;
            for (int i = p + cols; i > p; i--) if (e->buf[i - 1] == ' ') { b = i; break; }
            int seg = b > 0 ? b - p : cols;
            vl[n++] = (VLine){ p, seg, ls, first ? box : 0 };
            p += seg; first = 0;
        }
        vl[n++] = (VLine){ p, le - p, ls, first ? box : 0 };
        pos = le + 1;
        if (le >= e->len) break;
    }
    return n;
}

static int ui_edit_cursor_vline(const UiEdit *e, const VLine *vl, int n) {
    for (int i = 0; i < n; i++) {
        int end = vl[i].start + vl[i].len;
        if (e->cur >= vl[i].start && (e->cur < end || (e->cur == end && (i == n - 1 || vl[i + 1].start != e->cur)))) return i;
    }
    return n - 1;
}

static void ui_edit_insert(UiEdit *e, const char *s, int n) {
    if (e->len + n > e->cap) return;
    memmove(e->buf + e->cur + n, e->buf + e->cur, e->len - e->cur + 1);
    memcpy(e->buf + e->cur, s, n); e->len += n; e->cur += n; e->edited = 1; e->reveal = 1;
}
static void ui_edit_delete(UiEdit *e, int from, int n) {
    if (from < 0 || from + n > e->len) return;
    memmove(e->buf + from, e->buf + from + n, e->len - from - n + 1); e->len -= n;
    if (e->cur > from) e->cur = e->cur >= from + n ? e->cur - n : from;
    e->edited = 1; e->reveal = 1;
}

static void ui_edit_line_bounds(const UiEdit *e, int at, int *ls, int *le) {
    int a = at; while (a > 0 && e->buf[a - 1] != '\n') a--;
    int b = at; while (b < e->len && e->buf[b] != '\n') b++;
    *ls = a; *le = b;
}

// Insert or remove a tick-box at the start of the cursor's line.
static void ui_edit_toggle_box_prefix(UiEdit *e) {
    int ls, le; ui_edit_line_bounds(e, e->cur, &ls, &le);
    int c = e->cur;
    if (ui_is_box(e->buf, ls, le)) { ui_edit_delete(e, ls, 4); e->cur = c - 4 < ls ? ls : c - 4; }
    else { e->cur = ls; ui_edit_insert(e, "[ ] ", 4); e->cur = c + 4; }
}

// Handle one key code from the on-screen keyboard.
static void ui_edit_key(UiEdit *e, int code) {
    if (code == 0x7f) {                                              // backspace
        int ls, le; ui_edit_line_bounds(e, e->cur, &ls, &le);
        if (ui_is_box(e->buf, ls, le) && e->cur == ls + 4) { ui_edit_delete(e, ls, 4); e->cur = ls; }
        else if (e->cur > 0) ui_edit_delete(e, e->cur - 1, 1);
    } else if (code == '\r' || code == '\n') {
        int ls, le; ui_edit_line_bounds(e, e->cur, &ls, &le);
        if (ui_is_box(e->buf, ls, le)) {
            if (le == ls + 4 && e->cur == le) { ui_edit_delete(e, ls, 4); e->cur = ls; }       // empty item: leave the list
            else ui_edit_insert(e, "\n[ ] ", 5);                                              // next item
        } else ui_edit_insert(e, "\n", 1);
    } else if (code == '\t') ui_edit_insert(e, "  ", 2);
    else if (code >= 32 && code < 127) { char c = (char)code; ui_edit_insert(e, &c, 1); }
}

static void ui_edit_move(UiEdit *e, int d) { e->cur += d; if (e->cur < 0) e->cur = 0; if (e->cur > e->len) e->cur = e->len; e->reveal = 1; }

// Draw the editor in the region (x,y,w,h) at the given font scale, and handle taps (place the cursor, toggle tick-boxes).
// Returns 1 if the text changed because of a tick-box tap.
static int ui_edit_draw(Ui *u, UiEdit *e, int x, int y, int w, int h, int scale, int caret_on) {
    int cw = 8 * scale, rh = 16 * scale + 4, cols = w / cw;
    static VLine vl[UI_MAXV];
    int n = ui_edit_layout(e, cols, vl, UI_MAXV);
    int changed = 0;

    if (e->reveal) {
        int ci = ui_edit_cursor_vline(e, vl, n);
        if (ci * rh < e->scroll.pos) e->scroll.pos = ci * rh;
        if ((ci + 1) * rh > e->scroll.pos + h) e->scroll.pos = (ci + 1) * rh - h;
        e->reveal = 0;
    }
    ui_scroll(u, &e->scroll, x, y, w, h, n * rh + rh);

    if (ui_tapped_in(u, x, y, w, h)) {                               // place the cursor, or toggle a box
        int row = (u->tap_y - y + e->scroll.pos) / rh; if (row < 0) row = 0; if (row >= n) row = n - 1;
        int col = (u->tap_x - x + cw / 2) / cw; if (col < 0) col = 0;
        if (vl[row].box && col < 4 && u->tap_x - x < 4 * cw) {
            int at = vl[row].lstart + 1; e->buf[at] = e->buf[at] == ' ' ? 'x' : ' '; e->edited = 1; changed = 1;
        } else {
            if (col > vl[row].len) col = vl[row].len;
            e->cur = vl[row].start + col; e->reveal = 0;
        }
    }

    draw_rect(u->s, x, y, w, h, 0x141A22);
    int ci = ui_edit_cursor_vline(e, vl, n);
    for (int i = 0; i < n; i++) {
        int ty = y + i * rh - e->scroll.pos;
        if (ty + rh <= y || ty >= y + h) continue;
        int start = vl[i].start, len = vl[i].len, off = 0;
        if (vl[i].box) {                                              // draw the tick-box instead of "[ ] "
            int bx = x + 2, by = ty + 2, bs = 16 * scale - 2;
            draw_rect(u->s, bx, by, bs, bs, 0x8899AA); draw_rect(u->s, bx + 3, by + 3, bs - 6, bs - 6, vl[i].box == 2 ? 0x55CC77 : 0x141A22);
            start += 4; len -= 4; off = 4; if (len < 0) len = 0;
        }
        char line[512]; int m = len < 500 ? len : 500; memcpy(line, e->buf + start, m); line[m] = 0;
        draw_text(u->s, x + off * cw, ty + 2, scale, line, vl[i].box == 2 ? 0x778899 : 0xE8E8E8);
        if (vl[i].box == 2) draw_rect(u->s, x + off * cw, ty + 2 + 8 * scale, m * cw, 2, 0x778899);   // strike through
        if (caret_on && i == ci) {
            int col = e->cur - vl[i].start; if (col < 0) col = 0;
            draw_rect(u->s, x + col * cw, ty + 2, 2, 16 * scale, 0x77CCFF);
        }
    }
    return changed;
}

// ---------------------------------------------------------------- keyboard

// Read touches on the on-screen keyboard. Keys fire on touch-down. Writes up to `max` key codes into out[].
// Backspace repeats while held.
static int ui_keyboard(Ui *u, Kbd *kb, const PlatTouch *t, int n, int *out, int max) {
    int cur[16] = {0}, got = 0; double now = ui_now();
    for (int i = 0; i < n; i++) {
        int id = t[i].id & 15; cur[id] = 1;
        if (!u->kb_prev[id]) {
            int c = kbd_press(kb, id, t[i].x, t[i].y);
            if (c >= 0 && got < max) out[got++] = c;
            u->kb_t[id] = now; u->kb_rep[id] = now + 0.45;
        } else if (kbd_held_code(kb, id) == K_BKSP && now >= u->kb_rep[id] && got < max) {
            out[got++] = 0x7f; u->kb_rep[id] = now + 0.07;
        }
    }
    for (int id = 0; id < 16; id++) if (u->kb_prev[id] && !cur[id]) kbd_release(kb, id);
    memcpy(u->kb_prev, cur, sizeof cur);
    return got;
}

#endif
