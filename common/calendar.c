// calendar.c - a month calendar for the iOS and Android framebuffers.
// usage: calendar [seconds] [calendar-dir] [notes-dir]      (defaults: 86400, DATA/calendar, DATA/notes)
// Tap a day to see it. A day shows your own events (DIR/events.txt, one line "YYYY-MM-DD text" each) and the to-do items of your notes
// that end with " @YYYY-MM-DD" (set with the DATE button in Notes). Tap a to-do tick box to tick it off in its note.
#include "plat.h"
#include "draw.h"
#include "kbd.h"
#include "ui.h"
#include "caldate.h"
#include <dirent.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __APPLE__
#define DEFAULT_CAL "/var/root/data/calendar"
#define DEFAULT_NOTES "/var/root/data/notes"
#else
#define DEFAULT_CAL "/data/local/tmp/data/calendar"
#define DEFAULT_NOTES "/data/local/tmp/data/notes"
#endif

#define MAXEV 1500
typedef struct { CDate d; char text[100]; int line; } Event;                       // line: its number in events.txt
typedef struct { CDate d; char text[100]; char note[64]; int off; int done; } Todo;
static Event ev[MAXEV]; static int nev;
static Todo td[MAXEV]; static int ntd;
static char caldir[256] = DEFAULT_CAL, notesdir[256] = DEFAULT_NOTES;

static void mkdirs(const char *p) {
    char tmp[256]; snprintf(tmp, sizeof tmp, "%s", p);
    for (char *c = tmp + 1; *c; c++) if (*c == '/') { *c = 0; mkdir(tmp, 0755); *c = '/'; }
    mkdir(tmp, 0755);
}
static void events_path(char *o, size_t n) { snprintf(o, n, "%s/events.txt", caldir); }

static void load_all(void) {
    nev = ntd = 0;
    char p[300]; events_path(p, sizeof p);
    FILE *f = fopen(p, "r");
    if (f) {
        char line[200]; int ln = 0;
        while (fgets(line, sizeof line, f) && nev < MAXEV) {
            line[strcspn(line, "\r\n")] = 0;
            CDate d; if (cd_parse(line, &d) && line[10] == ' ') { ev[nev].d = d; snprintf(ev[nev].text, sizeof ev[0].text, "%s", line + 11); ev[nev].line = ln; nev++; }
            ln++;
        }
        fclose(f);
    }
    DIR *dr = opendir(notesdir); if (!dr) return;
    struct dirent *e;
    while ((e = readdir(dr)) && ntd < MAXEV) {
        size_t l = strlen(e->d_name);
        if (e->d_name[0] == '.' || l < 5 || strcmp(e->d_name + l - 4, ".txt") || l > 60) continue;
        snprintf(p, sizeof p, "%s/%s", notesdir, e->d_name);
        FILE *nf = fopen(p, "rb"); if (!nf) continue;
        fseek(nf, 0, SEEK_END); long n = ftell(nf); fseek(nf, 0, SEEK_SET);
        if (n > 80000) n = 80000;
        char *b = malloc((size_t)n + 1); if (!b) { fclose(nf); continue; }
        n = (long)fread(b, 1, (size_t)n, nf); b[n] = 0; fclose(nf);
        for (char *s = b; s && ntd < MAXEV; s = strchr(s, '\n') ? strchr(s, '\n') + 1 : NULL) {
            if (s[0] != '[' || (s[1] != ' ' && s[1] != 'x') || s[2] != ']' || s[3] != ' ') continue;
            char *le = strchr(s, '\n'); int len = le ? (int)(le - s) : (int)strlen(s);
            CDate d; int at; if (!cd_line_due(s, len, &d, &at)) continue;
            Todo *t = &td[ntd++]; t->d = d; t->off = (int)(s - b); t->done = s[1] == 'x';
            int tl = at - 4; if (tl < 0) tl = 0; if (tl > (int)sizeof t->text - 1) tl = (int)sizeof t->text - 1;
            memcpy(t->text, s + 4, (size_t)tl); t->text[tl] = 0;
            snprintf(t->note, sizeof t->note, "%s", e->d_name);
        }
        free(b);
    }
    closedir(dr);
}
static int count_day(CDate d) { int c = 0; for (int i = 0; i < nev; i++) if (cd_same(ev[i].d, d)) c++; for (int i = 0; i < ntd; i++) if (cd_same(td[i].d, d)) c++; return c; }

static void toggle_todo(const Todo *t) {                                         // tick or untick the line in its note
    char p[300]; snprintf(p, sizeof p, "%s/%s", notesdir, t->note);
    FILE *f = fopen(p, "rb"); if (!f) return;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1); if (!b) { fclose(f); return; }
    n = (long)fread(b, 1, (size_t)n, f); fclose(f);
    if (t->off + 3 < n && (b[t->off + 1] == ' ' || b[t->off + 1] == 'x')) {
        b[t->off + 1] = b[t->off + 1] == ' ' ? 'x' : ' ';
        f = fopen(p, "wb"); if (f) { fwrite(b, 1, (size_t)n, f); fclose(f); }
    }
    free(b);
}
static void add_event(CDate d, const char *text) {
    mkdirs(caldir); char p[300]; events_path(p, sizeof p);
    FILE *f = fopen(p, "a"); if (!f) return;
    char ds[16]; cd_fmt(d, ds); fprintf(f, "%s %s\n", ds, text); fclose(f);
}
static void delete_event(int line) {                                              // rewrite events.txt without that line
    char p[300]; events_path(p, sizeof p);
    FILE *f = fopen(p, "r"); if (!f) return;
    static char keep[MAXEV][200]; int nk = 0, ln = 0; char l[200];
    while (fgets(l, sizeof l, f) && nk < MAXEV) { if (ln++ != line) snprintf(keep[nk++], sizeof keep[0], "%s", l); }
    fclose(f);
    f = fopen(p, "w"); if (!f) return;
    for (int i = 0; i < nk; i++) fputs(keep[i], f);
    fclose(f);
}

enum { V_MONTH, V_DAY, V_ADD };

int main(int argc, char **argv) {
    double seconds = argc > 1 ? atof(argv[1]) : 86400;
    if (argc > 2) snprintf(caldir, sizeof caldir, "%s", argv[2]);
    if (argc > 3) snprintf(notesdir, sizeof notesdir, "%s", argv[3]);
    PlatInfo pi;
    if (plat_init(&pi)) return 1;
    if (plat_touch_init(&pi)) return 1;
    int W = pi.w, H = pi.h;
    mkdirs(caldir);
    Ui u; memset(&u, 0, sizeof u);
    int key_h = W / 8, kbd_h = key_h * 5 + 6, kb_y = H - kbd_h;
    Kbd kb; kbd_init(&kb, 0, kb_y, W, kbd_h);
    UiEdit ed; ui_edit_init(&ed, 90);
    UiScroll day_sc = {0};
    CDate today = cd_today(), shown = today, day = today;
    int view = V_MONTH, quit = 0, dirty = 3, confirm = -1;
    double t0 = ui_now(), t_blink = t0; int blink = 1;
    const int BAR = 64, ROW = 76;
    load_all();

    while (!quit && ui_now() - t0 < seconds) {
        double t = ui_now();
        PlatTouch tc[10];
        int n = plat_touch_poll(tc, 10);
        ui_poll(&u, tc, n);
        int keys[16], nk = 0;
        if (view == V_ADD) {
            nk = ui_keyboard(&u, &kb, tc, n, keys, 16);
            for (int i = 0; i < nk; i++) { if (keys[i] == '\n' || keys[i] == '\r') { if (ed.len) { add_event(day, ed.buf); load_all(); } view = V_DAY; } else ui_edit_key(&ed, keys[i]); }
            if (nk) { dirty = 3; blink = 1; t_blink = t; }
            if (kb.dirty) { dirty = 3; kb.dirty = 0; }
        }
        if (t - t_blink >= 0.5) { blink ^= 1; t_blink = t; if (view == V_ADD) dirty = 3; }
        if (u.changed || u.tapped) dirty = 3;
        { CDate nt = cd_today(); if (!cd_same(nt, today)) { today = nt; dirty = 3; } }
        if (dirty <= 0) { usleep(8000); continue; }
        dirty--;

        int stride; uint32_t *px = plat_begin(&stride);
        Surf s = { px, stride, W, H, pi.rs, pi.gs, pi.bs, pi.alpha, pi.theme };
        u.s = &s; u.W = W; u.H = H;
        draw_rect(&s, 0, 0, W, H, 0x10161E);
        draw_rect(&s, 0, 0, W, BAR, 0x26303E);

        if (view == V_MONTH) {
            if (ui_button(&u, 6, 8, 60, BAR - 16, "<", 0x34608F)) { if (--shown.m < 1) { shown.m = 12; shown.y--; } load_all(); }
            if (ui_button(&u, 72, 8, 60, BAR - 16, ">", 0x34608F)) { if (++shown.m > 12) { shown.m = 1; shown.y++; } load_all(); }
            char hd[48]; snprintf(hd, sizeof hd, "%.3s %d", CD_MON[shown.m - 1], shown.y);
            ui_label(&u, 142, 16, 2, hd, 0xFFFFFF);
            if (ui_button(&u, W - 232, 8, 112, BAR - 16, "TODAY", 0x2E7D4F)) { shown = today; }
            if (ui_button(&u, W - 112, 8, 104, BAR - 16, "EXIT", 0x883333)) quit = 1;
            int cw = W / 7, gx = (W - cw * 7) / 2, ch = (H - BAR - 30) / 6; if (ch > 150) ch = 150;
            for (int i = 0; i < 7; i++) ui_label(&u, gx + i * cw + (cw - 16) / 2, BAR + 8, 1, CD_DOW[i], 0x8899AA);
            int first = cd_dow(shown.y, shown.m, 1), dim = cd_dim(shown.y, shown.m);
            for (int d = 1; d <= dim; d++) {
                int idx = first + d - 1, cx = gx + (idx % 7) * cw, cy = BAR + 30 + (idx / 7) * ch;
                CDate c = { shown.y, shown.m, d };
                int is_today = cd_same(c, today), cnt = count_day(c);
                draw_rect(&s, cx + 2, cy + 2, cw - 4, ch - 4, is_today ? 0x34608F : 0x18202A);
                char nn[4]; snprintf(nn, sizeof nn, "%d", d);
                ui_label(&u, cx + 8, cy + 8, 2, nn, 0xF0F0F0);
                if (cnt) {                                                          // dots for the items of the day, up to five
                    int dots = cnt > 5 ? 5 : cnt;
                    for (int k = 0; k < dots; k++) draw_rect(&s, cx + 8 + k * 12, cy + ch - 20, 8, 8, 0x55CC77);
                    if (cnt > 5) ui_label(&u, cx + 8 + 5 * 12, cy + ch - 24, 1, "+", 0x55CC77);
                }
                if (ui_tapped_in(&u, cx, cy, cw, ch)) { day = c; day_sc.pos = 0; confirm = -1; view = V_DAY; u.tapped = 0; dirty = 3; }
            }
        } else if (view == V_DAY) {
            if (ui_button(&u, 6, 8, 120, BAR - 16, "BACK", 0x34608F)) { shown.y = day.y; shown.m = day.m; view = V_MONTH; }
            char hd[48]; snprintf(hd, sizeof hd, "%s %.3s %d", CD_DOW[cd_dow(day.y, day.m, day.d)], CD_MON[day.m - 1], day.d);
            ui_label(&u, 140, 16, 2, hd, 0xFFFFFF);
            int ly = BAR, lh = H - BAR - 80, rows = 0;
            for (int i = 0; i < nev; i++) if (cd_same(ev[i].d, day)) rows++;
            for (int i = 0; i < ntd; i++) if (cd_same(td[i].d, day)) rows++;
            ui_scroll(&u, &day_sc, 0, ly, W, lh, rows * ROW);
            int r = 0;
            for (int i = 0; i < nev; i++) {
                if (!cd_same(ev[i].d, day)) continue;
                int y = ly + r * ROW - day_sc.pos; r++;
                if (y + ROW <= ly || y >= ly + lh) continue;
                char b[110]; draw_rect(&s, 0, y, W, ROW - 2, 0x18202A);
                draw_rect(&s, 16, y + 22, 24, 24, 0x55CC77);
                ui_fit(b, sizeof b, ev[i].text, (W - 200) / 16); ui_label(&u, 56, y + 22, 2, b, 0xF0F0F0);
                int armed = confirm == i;
                if (ui_button(&u, W - 124, y + 10, 112, ROW - 22, armed ? "SURE?" : "DEL", 0x883333)) {
                    if (armed) { delete_event(ev[i].line); load_all(); confirm = -1; dirty = 3; break; } else confirm = i;
                }
            }
            for (int i = 0; i < ntd; i++) {
                if (!cd_same(td[i].d, day)) continue;
                int y = ly + r * ROW - day_sc.pos; r++;
                if (y + ROW <= ly || y >= ly + lh) continue;
                char b[110]; draw_rect(&s, 0, y, W, ROW - 2, 0x18202A);
                draw_rect(&s, 16, y + 18, 36, 36, 0x8899AA); draw_rect(&s, 19, y + 21, 30, 30, td[i].done ? 0x55CC77 : 0x10161E);
                ui_fit(b, sizeof b, td[i].text, (W - 100) / 16); ui_label(&u, 68, y + 8, 2, b, td[i].done ? 0x778899 : 0xF0F0F0);
                char nb[70]; snprintf(nb, sizeof nb, "note: %s", td[i].note); ui_fit(b, sizeof b, nb, (W - 100) / 8); ui_label(&u, 68, y + 48, 1, b, 0x778899);
                if (u.tapped && u.tap_y >= ly && ui_inside(u.tap_x, u.tap_y, 0, y, 140, ROW)) { toggle_todo(&td[i]); load_all(); u.tapped = 0; dirty = 3; break; }
            }
            if (!rows) ui_label(&u, 16, ly + 24, 2, "Nothing on this day.", 0x8899AA);
            if (ui_button(&u, 8, H - 72, W - 16, 64, "+ EVENT", 0x2E7D4F)) { ui_edit_set(&ed, ""); view = V_ADD; }
        } else {
            char hd[48]; snprintf(hd, sizeof hd, "NEW EVENT %.3s %d", CD_MON[day.m - 1], day.d);
            ui_label(&u, 16, 20, 2, hd, 0xFFFFFF);
            if (ui_button(&u, W - 136, 8, 128, BAR - 16, "CANCEL", 0x883333)) view = V_DAY;
            if (ui_button(&u, W - 272, 8, 128, BAR - 16, "SAVE", 0x2E7D4F) && ed.len) { add_event(day, ed.buf); load_all(); view = V_DAY; }
            ui_edit_draw(&u, &ed, 0, BAR, W, kb_y - BAR, 2, blink);
            kbd_render(&kb, &s);
        }
        plat_flip();
    }
    plat_close();
    return 0;
}
