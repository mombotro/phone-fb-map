// caldate.h - dates, and a date picker, for Notes and Calendar. Include after ui.h.
// A to-do line in a note may end with " @YYYY-MM-DD" (for example "[ ] Pay rent @2026-11-01"). The Calendar shows such lines on their day.
#ifndef CALDATE_H
#define CALDATE_H
#include <stdio.h>
#include <string.h>
#include <time.h>

typedef struct { int y, m, d; } CDate;                                  // m is 1 to 12, d is 1 to 31

static const char *CD_MON[12] = { "JANUARY", "FEBRUARY", "MARCH", "APRIL", "MAY", "JUNE", "JULY", "AUGUST", "SEPTEMBER", "OCTOBER", "NOVEMBER", "DECEMBER" };
static const char *CD_DOW[7] = { "SU", "MO", "TU", "WE", "TH", "FR", "SA" };

static int cd_leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
static int cd_dim(int y, int m) { static const int d[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 }; return m == 2 && cd_leap(y) ? 29 : d[(m - 1) % 12]; }
static int cd_dow(int y, int m, int d) {                                 // 0 is Sunday
    static const int t[12] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (m < 3) y--;
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}
static CDate cd_today(void) { time_t t = time(NULL); struct tm lt; localtime_r(&t, &lt); CDate c = { lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday }; return c; }
static int cd_same(CDate a, CDate b) { return a.y == b.y && a.m == b.m && a.d == b.d; }
static void cd_fmt(CDate d, char *out) { sprintf(out, "%04d-%02d-%02d", d.y, d.m, d.d); }
// "YYYY-MM-DD" at s. Returns 1 when it is a real date.
static int cd_parse(const char *s, CDate *d) {
    int y, m, dd;
    if (strlen(s) < 10 || s[4] != '-' || s[7] != '-') return 0;
    for (int i = 0; i < 10; i++) if (i != 4 && i != 7 && (s[i] < '0' || s[i] > '9')) return 0;
    y = atoi(s); m = atoi(s + 5); dd = atoi(s + 8);
    if (y < 1970 || y > 2200 || m < 1 || m > 12 || dd < 1 || dd > cd_dim(y, m)) return 0;
    d->y = y; d->m = m; d->d = dd; return 1;
}
// A line (len characters, no newline) that ends with " @YYYY-MM-DD". *at is the index of the space before the "@".
static int cd_line_due(const char *line, int len, CDate *d, int *at) {
    while (len > 0 && (line[len - 1] == ' ' || line[len - 1] == '\r')) len--;
    if (len < 12 || line[len - 11] != '@' || line[len - 12] != ' ') return 0;
    char b[16]; memcpy(b, line + len - 10, 10); b[10] = 0;
    if (!cd_parse(b, d)) return 0;
    if (at) *at = len - 12;
    return 1;
}

// ---- the date picker: a month grid with CLEAR, TODAY, and CANCEL
typedef struct { int y, m; CDate sel; int has_sel; } DatePick;
static void dp_init(DatePick *p, const CDate *start) {
    CDate t = cd_today(); p->has_sel = start != NULL;
    p->sel = start ? *start : t; p->y = p->sel.y; p->m = p->sel.m;
}
// Draws the picker in the area (x, y, w, h) and handles taps. Returns 0 (still open), 1 (a day was chosen, in p->sel), 2 (cancel), 3 (clear the date).
static int dp_run(Ui *u, DatePick *p, int x, int y, int w, int h) {
    draw_rect(u->s, x, y, w, h, 0x10161E);
    const int HEAD = 60, WK = 32, FOOT = 64;
    int cw = w / 7, rows = 6, ch = (h - HEAD - WK - FOOT - 8) / rows, gx = x + (w - cw * 7) / 2;
    int ret = 0;
    // header: previous month, the name, next month
    if (ui_button(u, x + 6, y + 6, 72, HEAD - 12, "<", 0x34608F)) { if (--p->m < 1) { p->m = 12; p->y--; } }
    if (ui_button(u, x + w - 78, y + 6, 72, HEAD - 12, ">", 0x34608F)) { if (++p->m > 12) { p->m = 1; p->y++; } }
    char hd[48]; snprintf(hd, sizeof hd, "%s %d", CD_MON[p->m - 1], p->y);
    ui_label(u, x + (w - (int)strlen(hd) * 16) / 2, y + 14, 2, hd, 0xFFFFFF);
    for (int i = 0; i < 7; i++) ui_label(u, gx + i * cw + (cw - 16) / 2, y + HEAD + 8, 1, CD_DOW[i], 0x8899AA);
    int first = cd_dow(p->y, p->m, 1), dim = cd_dim(p->y, p->m); CDate today = cd_today();
    for (int d = 1; d <= dim; d++) {
        int idx = first + d - 1, cx = gx + (idx % 7) * cw, cy = y + HEAD + WK + (idx / 7) * ch;
        CDate c = { p->y, p->m, d };
        int is_today = cd_same(c, today), is_sel = p->has_sel && cd_same(c, p->sel);
        draw_rect(u->s, cx + 2, cy + 2, cw - 4, ch - 4, is_sel ? 0x2E7D4F : is_today ? 0x34608F : 0x18202A);
        char n[4]; snprintf(n, sizeof n, "%d", d);
        ui_label(u, cx + (cw - (int)strlen(n) * 16) / 2, cy + (ch - 32) / 2, 2, n, 0xF0F0F0);
        if (ui_tapped_in(u, cx, cy, cw, ch)) { p->sel = c; p->has_sel = 1; u->tapped = 0; ret = 1; }
    }
    int by = y + h - FOOT + 4, bw = (w - 32) / 3;
    if (ui_button(u, x + 8, by, bw, FOOT - 12, "CLEAR", 0x883333)) ret = 3;
    if (ui_button(u, x + 16 + bw, by, bw, FOOT - 12, "TODAY", 0x34608F)) { p->y = today.y; p->m = today.m; }
    if (ui_button(u, x + 24 + 2 * bw, by, bw, FOOT - 12, "CANCEL", 0x34608F)) ret = 2;
    return ret;
}
#endif
