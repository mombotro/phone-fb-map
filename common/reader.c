// reader.c - read .txt, .epub, and .pdf books, for the iOS and Android framebuffers.
// usage: reader [seconds] [dir]      (default 86400 seconds, dir DATA/books)
// Put the book files in the folder with Phone Files. The reader remembers where you stopped in each book.
#include "plat.h"
#include "draw.h"
#include "kbd.h"
#include "ui.h"
#include "epub.h"
#include "pdf.h"
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __APPLE__
#define DEFAULT_DIR "/var/root/data/books"
#else
#define DEFAULT_DIR "/data/local/tmp/data/books"
#endif

#define MAXBOOKS 300
typedef struct { char name[200]; int milli; } BookRow;           // milli: how far you read, 0 to 1000
static BookRow books[MAXBOOKS]; static int nbooks;
static char dir[256] = DEFAULT_DIR;

static char *text; static size_t tlen;
typedef struct { int start, len; } Ln;
static Ln *lines; static int nlines, lcap;

static int has_book_ext(const char *n) { return eb_has_ext(n, ".txt") || eb_has_ext(n, ".epub") || eb_has_ext(n, ".pdf") || eb_has_ext(n, ".md"); }
static int cmp_book(const void *a, const void *b) { return strcasecmp(((const BookRow *)a)->name, ((const BookRow *)b)->name); }

// ---- where you stopped: one line "milli<TAB>file name" per book, in DIR/.pos
static void pos_path(char *out, size_t n) { snprintf(out, n, "%s/.pos", dir); }
static int pos_get(const char *name) {
    char p[300], line[400]; pos_path(p, sizeof p);
    FILE *f = fopen(p, "r"); if (!f) return 0;
    int r = 0;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *tab = strchr(line, '\t'); if (!tab) continue;
        if (strcmp(tab + 1, name) == 0) { r = atoi(line); break; }
    }
    fclose(f); return r;
}
static void pos_set(const char *name, int milli) {
    char p[300], line[400]; pos_path(p, sizeof p);
    static char keep[MAXBOOKS][400]; int nk = 0;
    FILE *in = fopen(p, "r");
    if (in) {
        while (nk < MAXBOOKS && fgets(line, sizeof line, in)) {
            char tmp[400]; snprintf(tmp, sizeof tmp, "%s", line); tmp[strcspn(tmp, "\r\n")] = 0;
            char *tab = strchr(tmp, '\t'); if (!tab || strcmp(tab + 1, name) == 0) continue;
            snprintf(keep[nk++], sizeof keep[0], "%s", line);
        }
        fclose(in);
    }
    FILE *out = fopen(p, "w"); if (!out) return;                   // written in place: rename() does not work on old Android kernels
    for (int i = 0; i < nk; i++) fputs(keep[i], out);
    fprintf(out, "%d\t%s\n", milli, name); fclose(out);
}

static void scan_books(void) {
    nbooks = 0;
    DIR *d = opendir(dir); if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && nbooks < MAXBOOKS) {
        if (e->d_name[0] == '.' || !has_book_ext(e->d_name)) continue;
        snprintf(books[nbooks].name, sizeof books[0].name, "%s", e->d_name);
        books[nbooks].milli = pos_get(e->d_name); nbooks++;
    }
    closedir(d);
    qsort(books, (size_t)nbooks, sizeof books[0], cmp_book);
}

// Break the text into screen lines of `cols` characters, at spaces when it can.
static void layout(int cols) {
    nlines = 0;
    size_t i = 0;
    while (i <= tlen) {
        size_t e = i; while (e < tlen && text[e] != '\n') e++;      // [i, e) is one paragraph line
        size_t p = i;
        if (e == i) { /* blank line */ }
        while (p < e || (e == i && p == i)) {
            size_t n = e - p;
            if (n > (size_t)cols) { n = (size_t)cols; size_t k = n; while (k > 0 && text[p + k] != ' ') k--; if (k > 0) n = k; }
            if (nlines == lcap) { lcap = lcap ? lcap * 2 : 4096; lines = realloc(lines, (size_t)lcap * sizeof *lines); if (!lines) { nlines = 0; return; } }
            lines[nlines].start = (int)p; lines[nlines].len = (int)n; nlines++;
            p += n; while (p < e && text[p] == ' ') p++;
            if (e == i) break;
        }
        if (e >= tlen) break;
        i = e + 1;
    }
}
static int line_at(size_t off) {                                   // the line that holds a character offset
    int lo = 0, hi = nlines - 1;
    while (lo < hi) { int mid = (lo + hi + 1) / 2; if ((size_t)lines[mid].start <= off) lo = mid; else hi = mid - 1; }
    return lo;
}

enum { V_LIB, V_READ };

int main(int argc, char **argv) {
    double seconds = argc > 1 ? atof(argv[1]) : 86400;
    if (argc > 2) snprintf(dir, sizeof dir, "%s", argv[2]);
    PlatInfo pi;
    if (plat_init(&pi)) return 1;
    if (plat_touch_init(&pi)) return 1;
    int W = pi.w, H = pi.h;
    Ui u; memset(&u, 0, sizeof u);
    UiScroll sc_lib = {0}, sc_read = {0};
    int view = V_LIB, quit = 0, dirty = 3, fs = 2, cur = -1, loading = 0;
    double t0 = ui_now();
    const int BAR = 64, FOOT = 64, ROW = 84;
    int laid_fs = 0, lay_rh = 20;                                  // lay_rh: the line height the current layout was made for
    char status[96] = "";

    { char p[300]; snprintf(p, sizeof p, "%s/.size", dir); FILE *f = fopen(p, "r"); if (f) { int v; if (fscanf(f, "%d", &v) == 1 && v >= 1 && v <= 3) fs = v; fclose(f); } }
    mkdir(dir, 0755);
    scan_books();

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

        if (view == V_LIB) {
            ui_label(&u, 16, 16, 2, "BOOKS", 0xFFFFFF);
            if (ui_button(&u, W - 136, 8, 128, BAR - 16, "EXIT", 0x883333)) quit = 1;
            int ly = BAR, lh = H - BAR;
            ui_scroll(&u, &sc_lib, 0, ly, W, lh, nbooks * ROW);
            for (int i = 0; i < nbooks; i++) {
                int y = ly + i * ROW - sc_lib.pos;
                if (y + ROW <= ly || y >= ly + lh) continue;
                char buf[128];
                draw_rect(&s, 0, y, W, ROW - 2, 0x18202A);
                snprintf(buf, sizeof buf, "%s", books[i].name); char *dot = strrchr(buf, '.'); if (dot) *dot = 0;
                char fit[128]; ui_fit(fit, sizeof fit, buf, (W - 40) / 16);
                ui_label(&u, 16, y + 10, 2, fit, 0xF0F0F0);
                snprintf(buf, sizeof buf, "%s  -  %s", eb_has_ext(books[i].name, ".epub") ? "EPUB" : eb_has_ext(books[i].name, ".pdf") ? "PDF" : "text", books[i].milli > 0 ? "" : "not started");
                if (books[i].milli > 0) snprintf(buf, sizeof buf, "%s  -  read %d%%", eb_has_ext(books[i].name, ".epub") ? "EPUB" : eb_has_ext(books[i].name, ".pdf") ? "PDF" : "text", books[i].milli / 10);
                ui_label(&u, 16, y + 50, 1, buf, 0x8899AA);
                if (u.tapped && u.tap_y >= ly && ui_inside(u.tap_x, u.tap_y, 0, y, W, ROW)) {
                    u.tapped = 0; cur = i; loading = 1; dirty = 3;
                }
            }
            if (!nbooks) {
                ui_label(&u, 16, ly + 24, 2, "No books yet.", 0xF0F0F0);
                ui_label(&u, 16, ly + 72, 1, "Copy .txt, .epub, and .pdf files to this folder with Phone Files:", 0x8899AA);
                ui_label(&u, 16, ly + 96, 1, dir, 0x8899AA);
            }
            if (status[0]) ui_label(&u, 16, H - 28, 1, status, 0xFF8888);
        } else {
            int cols = (W - 24) / (8 * fs), rh = 16 * fs + 4;
            if (laid_fs != fs) {                                   // first time, or the text size changed: keep the place
                int top = sc_read.pos / lay_rh; size_t off = (nlines && top < nlines) ? (size_t)lines[top].start : 0;
                layout(cols); laid_fs = fs; lay_rh = rh;
                sc_read.pos = nlines ? line_at(off) * rh : 0;
            }
            int ly = BAR, lh = H - BAR - FOOT;
            ui_scroll(&u, &sc_read, 0, ly, W, lh, nlines * rh + 16);
            int first = (sc_read.pos - 8) / rh; if (first < 0) first = 0;
            for (int i = first; i < nlines; i++) {
                int y = ly + 8 + i * rh - sc_read.pos;
                if (y >= ly + lh) break;
                if (y + rh <= ly || !lines[i].len) continue;
                char buf[256]; int n2 = lines[i].len < 255 ? lines[i].len : 255;
                memcpy(buf, text + lines[i].start, (size_t)n2); buf[n2] = 0;
                draw_text(&s, 12, y, fs, buf, 0xE8E8E8);
            }
            draw_rect(&s, 0, 0, W, BAR, 0x26303E);                  // bars on top so the text slides under them
            draw_rect(&s, 0, H - FOOT, W, FOOT, 0x26303E);
            if (ui_button(&u, 8, 8, 128, BAR - 16, "BOOKS", 0x34608F)) {
                int top = sc_read.pos / rh; books[cur].milli = nlines > 1 ? (top >= nlines ? 1000 : top * 1000 / (nlines - 1)) : 0;
                pos_set(books[cur].name, books[cur].milli); free(text); text = NULL; view = V_LIB; dirty = 3;
            }
            if (view == V_READ) {
                if (ui_button(&u, W - 80, 8, 72, BAR - 16, "A", 0x2E7D4F)) {
                    fs = fs % 3 + 1; dirty = 3;
                    char p[300]; snprintf(p, sizeof p, "%s/.size", dir); FILE *f = fopen(p, "w"); if (f) { fprintf(f, "%d\n", fs); fclose(f); }
                }
                char t[96], buf[128]; snprintf(buf, sizeof buf, "%s", books[cur].name); char *dot = strrchr(buf, '.'); if (dot) *dot = 0;
                ui_fit(t, sizeof t, buf, (W - 240) / 16);
                ui_label(&u, 148, 16, 2, t, 0xFFFFFF);
                // footer: page back, a progress bar you can tap, page forward
                int page = (lh / rh - 1) * rh; if (page < rh) page = rh;
                int bw = 96, bx0 = 8 + bw + 8, bx1 = W - 8 - bw - 8;
                if (ui_button(&u, 8, H - FOOT + 8, bw, FOOT - 16, "<<", 0x34608F)) { sc_read.pos -= page; dirty = 3; }
                if (ui_button(&u, W - 8 - bw, H - FOOT + 8, bw, FOOT - 16, ">>", 0x34608F)) { sc_read.pos += page; dirty = 3; }
                int maxpos = nlines * rh + 16 - lh; if (maxpos < 1) maxpos = 1;
                draw_rect(&s, bx0, H - FOOT / 2 - 4, bx1 - bx0, 8, 0x8899AA);
                int kx = bx0 + (int)((long)(bx1 - bx0) * sc_read.pos / maxpos);
                draw_rect(&s, kx - 8, H - FOOT / 2 - 18, 16, 36, 0xFFFFFF);
                if (u.tapped && ui_inside(u.tap_x, u.tap_y, bx0 - 10, H - FOOT, bx1 - bx0 + 20, FOOT)) {
                    int x = u.tap_x < bx0 ? bx0 : u.tap_x > bx1 ? bx1 : u.tap_x;
                    sc_read.pos = (int)((long)maxpos * (x - bx0) / (bx1 - bx0)); u.tapped = 0; dirty = 3;
                }
            }
        }
        plat_flip();

        if (loading) {                                              // a book is chosen: show the screen once, then load (a big EPUB takes a while)
            loading = 0; status[0] = 0;
            draw_rect(&s, 0, BAR, W, H - BAR, 0x10161E);
            ui_label(&u, 16, BAR + 40, 2, "Opening...", 0xFFFFFF);
            plat_flip();
            char path[512]; snprintf(path, sizeof path, "%s/%s", dir, books[cur].name);
            free(text); pdf_error[0] = 0;
            text = eb_has_ext(path, ".pdf") ? pdf_text(path, &tlen) : book_load(path, &tlen);
            if (!text || !tlen) { free(text); text = NULL; snprintf(status, sizeof status, "%s: %s", books[cur].name, pdf_error[0] ? pdf_error : "cannot read it"); }
            else {
                view = V_READ; laid_fs = 0; nlines = 0; sc_read.pos = 0;
                int cols = (W - 24) / (8 * fs); layout(cols); laid_fs = fs;
                int rh = 16 * fs + 4; lay_rh = rh;
                sc_read.pos = nlines > 1 ? (int)((long)books[cur].milli * (nlines - 1) / 1000) * rh : 0;
            }
            dirty = 3;
        }
    }
    if (view == V_READ && cur >= 0) { int rh = 16 * fs + 4, top = sc_read.pos / rh; pos_set(books[cur].name, nlines > 1 ? top * 1000 / (nlines - 1) : 0); }
    plat_close();
    return 0;
}
