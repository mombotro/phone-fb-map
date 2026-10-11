// bible.c - read the American Standard Version Bible, for the iOS and Android framebuffers.
// usage: bible [seconds] [file]      (default 86400 seconds, file DATA/bible/asv.txt, made by tools/make_bible.py)
// The file has a line "=Book name" before each book and one line "chapter:verse text" for each verse.
#include "plat.h"
#include "draw.h"
#include "kbd.h"
#include "ui.h"
#include <unistd.h>

#ifdef __APPLE__
#define DEFAULT_DIR "/var/root/data/bible"
#else
#define DEFAULT_DIR "/data/local/tmp/data/bible"
#endif

#define MAXB 70
#define MAXCH 154
#define MAXLINES 8000

typedef struct { char name[32]; int nch; int cstart[MAXCH]; } Book;     // cstart[c]: first verse of chapter c, cstart[nch + 1]: end
typedef struct { int verse; int off; int len; } Line;                   // verse is the verse number on the first line, else 0

static Book books[MAXB]; static int nbooks;
static char *text; static const char **vtext; static int *vnum; static int nverses;
static Line lines[MAXLINES]; static int nlines, indent;
static char dir[256] = DEFAULT_DIR;

static int load_bible(const char *path) {
    FILE *f = fopen(path, "rb"); if (!f) return -1;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    text = malloc(n + 1); if (!text) { fclose(f); return -1; }
    n = (long)fread(text, 1, n, f); text[n] = 0; fclose(f);
    int rows = 0; for (long i = 0; i < n; i++) if (text[i] == '\n') rows++;
    vtext = malloc(sizeof *vtext * (rows + 1)); vnum = malloc(sizeof *vnum * (rows + 1));
    if (!vtext || !vnum) return -1;
    Book *b = NULL; int lastc = 0;
    for (char *p = text; *p; ) {
        char *e = strchr(p, '\n'); if (e) *e = 0;
        if (p[0] == '=') {
            if (b) b->cstart[b->nch + 1] = nverses;
            if (nbooks >= MAXB) break;
            b = &books[nbooks++]; memset(b, 0, sizeof *b); snprintf(b->name, sizeof b->name, "%s", p + 1); lastc = 0;
        } else if (b && p[0]) {
            char *q; int c = (int)strtol(p, &q, 10); if (*q != ':') goto next;
            int v = (int)strtol(q + 1, &q, 10); while (*q == ' ') q++;
            if (c != lastc && c > 0 && c < MAXCH - 2) { b->cstart[c] = nverses; b->nch = c; lastc = c; }
            vtext[nverses] = q; vnum[nverses] = v; nverses++;
        }
next:   if (!e) break;
        p = e + 1;
    }
    if (b) b->cstart[b->nch + 1] = nverses;
    return nbooks ? 0 : -1;
}

// Break one chapter into screen lines of `cols` characters. A verse number sits in front of its first line, and the
// other lines of the verse are indented to the same place.
static void layout(int bk, int ch, int cols) {
    Book *b = &books[bk]; nlines = 0;
    int from = b->cstart[ch], to = b->cstart[ch + 1], maxv = 0;
    for (int i = from; i < to; i++) if (vnum[i] > maxv) maxv = vnum[i];
    indent = (maxv >= 100 ? 3 : maxv >= 10 ? 2 : 1) + 1;
    int avail = cols - indent; if (avail < 8) avail = 8;
    for (int i = from; i < to && nlines < MAXLINES - 1; i++) {
        const char *t = vtext[i]; int len = (int)strlen(t), pos = 0, first = 1;
        while (pos < len && nlines < MAXLINES - 1) {
            int n = len - pos;
            if (n > avail) { n = avail; int k = n; while (k > 0 && t[pos + k] != ' ') k--; if (k > 0) n = k; }
            lines[nlines].verse = first ? vnum[i] : 0; lines[nlines].off = (int)(t - text) + pos; lines[nlines].len = n; nlines++;
            pos += n; while (pos < len && t[pos] == ' ') pos++;
            first = 0;
        }
    }
}

static void save_pos(int bk, int ch, int sc) {
    char p[300]; snprintf(p, sizeof p, "%s/pos", dir);
    FILE *f = fopen(p, "w"); if (!f) return;
    fprintf(f, "%d %d %d\n", bk, ch, sc); fclose(f);
}
static void load_pos(int *bk, int *ch, int *sc) {
    char p[300]; snprintf(p, sizeof p, "%s/pos", dir);
    FILE *f = fopen(p, "r"); if (!f) return;
    int a, b, c; if (fscanf(f, "%d %d %d", &a, &b, &c) == 3 && a >= 0 && a < nbooks && b >= 1 && b <= books[a].nch && c >= 1 && c <= 3) { *bk = a; *ch = b; *sc = c; }
    fclose(f);
}

enum { V_BOOKS, V_CHAPTERS, V_READ };

int main(int argc, char **argv) {
    double seconds = argc > 1 ? atof(argv[1]) : 86400;
    char path[300]; snprintf(path, sizeof path, "%s/asv.txt", dir);
    if (argc > 2) { snprintf(path, sizeof path, "%s", argv[2]); char *sl = strrchr(path, '/'); if (sl) { snprintf(dir, sizeof dir, "%.*s", (int)(sl - path), path); } }
    int loaded = load_bible(path) == 0;
    PlatInfo pi;
    if (plat_init(&pi)) return 1;
    if (plat_touch_init(&pi)) return 1;
    int W = pi.w, H = pi.h;
    Ui u; memset(&u, 0, sizeof u);
    UiScroll sc_books = {0}, sc_ch = {0}, sc_read = {0};
    int view = V_BOOKS, quit = 0, dirty = 3, bk = 0, ch = 1, fs = 2;
    if (loaded) { load_pos(&bk, &ch, &fs); if (ch > 1 || bk > 0) view = V_READ; }
    double t0 = ui_now();
    const int BAR = 64, FOOT = 60, ROW = 60;
    int laid = 0;                                           // 1 when `lines` matches bk, ch, and fs

    while (!quit && ui_now() - t0 < seconds) {
        PlatTouch tc[10];
        int n = plat_touch_poll(tc, 10);
        ui_poll(&u, tc, n);
        if (u.changed || u.tapped) dirty = 3;
        if (dirty <= 0) { usleep(8000); continue; }
        dirty--;

        int stride; uint32_t *px = plat_begin(&stride);
        Surf s = { px, stride, W, H, pi.rs, pi.gs, pi.bs, pi.alpha, pi.theme };
        u.s = &s; u.W = W; u.H = H;
        draw_rect(&s, 0, 0, W, H, 0x10161E);
        draw_rect(&s, 0, 0, W, BAR, 0x26303E);

        if (!loaded) {
            ui_label(&u, 16, 16, 2, "BIBLE", 0xFFFFFF);
            ui_label(&u, 16, BAR + 24, 2, "No Bible text found.", 0xF0F0F0);
            ui_label(&u, 16, BAR + 72, 1, "Install it from Phone Files > FB Tools > Apps.", 0x8899AA);
            if (ui_button(&u, W - 136, 8, 128, BAR - 16, "EXIT", 0x883333)) quit = 1;
        } else if (view == V_BOOKS) {
            ui_label(&u, 16, 16, 2, "BIBLE", 0xFFFFFF);
            if (ui_button(&u, W - 136, 8, 128, BAR - 16, "EXIT", 0x883333)) quit = 1;
            int ly = BAR, lh = H - BAR, rows = nbooks + 2;
            ui_scroll(&u, &sc_books, 0, ly, W, lh, rows * ROW);
            for (int r = 0; r < rows; r++) {
                int ot = nbooks > 39 ? 39 : nbooks, y = ly + r * ROW - sc_books.pos;
                if (y + ROW <= ly || y >= ly + lh) continue;
                int idx = r == 0 ? -1 : r <= ot ? r - 1 : r == ot + 1 ? -2 : r - 2;
                if (idx < 0) {
                    draw_rect(&s, 0, y, W, ROW - 2, 0x2A3442);
                    ui_label(&u, 16, y + 14, 2, idx == -1 ? "OLD TESTAMENT" : "NEW TESTAMENT", 0xFFFFFF);
                    continue;
                }
                draw_rect(&s, 0, y, W, ROW - 2, 0x18202A);
                ui_label(&u, 16, y + 14, 2, books[idx].name, 0xF0F0F0);
                if (u.tapped && u.tap_y >= ly && ui_inside(u.tap_x, u.tap_y, 0, y, W, ROW)) { bk = idx; sc_ch.pos = 0; view = V_CHAPTERS; u.tapped = 0; dirty = 3; }
            }
        } else if (view == V_CHAPTERS) {
            char buf[64]; snprintf(buf, sizeof buf, "%s", books[bk].name);
            if (ui_button(&u, 8, 8, 128, BAR - 16, "BOOKS", 0x34608F)) { view = V_BOOKS; dirty = 3; }
            ui_fit(buf, sizeof buf, books[bk].name, (W - 160) / 16);
            ui_label(&u, 152, 16, 2, buf, 0xFFFFFF);
            int cols = W >= 560 ? 5 : 4, cell = (W - 16) / cols, ly = BAR, lh = H - BAR, rows = (books[bk].nch + cols - 1) / cols;
            ui_scroll(&u, &sc_ch, 0, ly, W, lh, rows * cell + 16);
            for (int c = 1; c <= books[bk].nch; c++) {
                int x = 8 + ((c - 1) % cols) * cell, y = ly + 8 + ((c - 1) / cols) * cell - sc_ch.pos;
                if (y + cell <= ly || y >= ly + lh) continue;
                draw_rect(&s, x + 3, y + 3, cell - 6, cell - 6, 0x34608F);
                draw_rect(&s, x + 6, y + 6, cell - 12, cell - 12, 0x18202A);
                snprintf(buf, sizeof buf, "%d", c);
                ui_label(&u, x + (cell - (int)strlen(buf) * 16) / 2, y + (cell - 32) / 2, 2, buf, 0xF0F0F0);
                if (u.tapped && u.tap_y >= ly && ui_inside(u.tap_x, u.tap_y, x, y, cell, cell)) { ch = c; laid = 0; sc_read.pos = 0; view = V_READ; u.tapped = 0; save_pos(bk, ch, fs); dirty = 3; }
            }
        } else {
            int cols = (W - 24) / (8 * fs), rh = 16 * fs + 6;
            if (!laid) { layout(bk, ch, cols); laid = 1; }
            int ly = BAR, lh = H - BAR - FOOT;
            ui_scroll(&u, &sc_read, 0, ly, W, lh, nlines * rh + 16);
            for (int i = 0; i < nlines; i++) {
                int y = ly + 8 + i * rh - sc_read.pos;
                if (y + rh <= ly || y >= ly + lh) continue;
                char buf[256]; int n2 = lines[i].len < 255 ? lines[i].len : 255;
                memcpy(buf, text + lines[i].off, n2); buf[n2] = 0;
                if (lines[i].verse) { char vb[8]; snprintf(vb, sizeof vb, "%d", lines[i].verse); draw_text(&s, 12, y, fs, vb, 0x7DA7DB); }
                draw_text(&s, 12 + indent * 8 * fs, y, fs, buf, 0xE8E8E8);
            }
            // the bars are drawn last so the text slides under them
            draw_rect(&s, 0, 0, W, BAR, 0x26303E);
            draw_rect(&s, 0, H - FOOT, W, FOOT, 0x26303E);
            char title[64]; snprintf(title, sizeof title, "%s %d", books[bk].name, ch);
            if (ui_button(&u, 8, 8, 128, BAR - 16, "BOOKS", 0x34608F)) { view = V_BOOKS; save_pos(bk, ch, fs); dirty = 3; }
            if (ui_button(&u, W - 80, 8, 72, BAR - 16, "A", 0x2E7D4F)) { fs = fs % 3 + 1; laid = 0; sc_read.pos = 0; save_pos(bk, ch, fs); dirty = 3; }
            ui_fit(title, sizeof title, title, (W - 240) / 16);
            ui_label(&u, 148, 16, 2, title, 0xFFFFFF);
            int bw = (W - 24) / 2;
            int prev_ok = ch > 1 || bk > 0, next_ok = ch < books[bk].nch || bk < nbooks - 1;
            if (prev_ok && ui_button(&u, 8, H - FOOT + 6, bw, FOOT - 12, "< PREV", 0x34608F)) {
                if (ch > 1) ch--; else { bk--; ch = books[bk].nch; }
                laid = 0; sc_read.pos = 0; save_pos(bk, ch, fs); dirty = 3;
            }
            if (next_ok && ui_button(&u, 16 + bw, H - FOOT + 6, bw, FOOT - 12, "NEXT >", 0x34608F)) {
                if (ch < books[bk].nch) ch++; else { bk++; ch = 1; }
                laid = 0; sc_read.pos = 0; save_pos(bk, ch, fs); dirty = 3;
            }
        }
        plat_flip();
    }
    if (loaded && view == V_READ) save_pos(bk, ch, fs);
    plat_close();
    return 0;
}
