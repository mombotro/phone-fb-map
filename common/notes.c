// notes.c - notes and to-do lists for the iOS and Android framebuffers.
// usage: notes [seconds] [dir]     (default 86400 seconds, dir /var/root/data/notes)
// Each note is one .txt file. The first line is the title. A line that starts with "[ ] " or "[x] " is a tick-box item.
#include "plat.h"
#include "draw.h"
#include "kbd.h"
#include "ui.h"
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#ifdef __ANDROID__
#include <sys/syscall.h>
#endif

#ifdef __APPLE__
#define DEFAULT_DIR "/var/root/data/notes"
#else
#define DEFAULT_DIR "/data/local/tmp/data/notes"
#endif

#define MAXN 400
#define MAXT 800
#define NOTE_CAP 60000

typedef struct { char name[64]; time_t mtime; char title[96]; char prev[96]; int open_items, all_items; } Note;
typedef struct { int note; int off; char text[120]; } Todo;

static char dir[256] = DEFAULT_DIR;
static Note notes[MAXN]; static int nnotes;
static Todo todos[MAXT]; static int ntodos;

static void mkdirs(const char *p) {
    char tmp[256]; snprintf(tmp, sizeof tmp, "%s", p);
    for (char *c = tmp + 1; *c; c++) if (*c == '/') { *c = 0; mkdir(tmp, 0755); *c = '/'; }
    mkdir(tmp, 0755);
}

// rename() from the NDK returns ENOSYS on Android 4.1 (checked on an LG Intuition), so call the kernel directly there.
static int move_file(const char *from, const char *to) {
#ifdef __ANDROID__
    return (int)syscall(__NR_rename, from, to);
#else
    return rename(from, to);
#endif
}

static char *read_file(const char *path, int *len) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n > NOTE_CAP) n = NOTE_CAP;
    char *b = malloc(n + 1); n = (long)fread(b, 1, n, f); b[n] = 0; fclose(f);
    if (len) *len = (int)n;
    return b;
}
static int write_file(const char *path, const char *data, int len) {
    char tmp[300]; snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb"); if (!f) return -1;
    fwrite(data, 1, len, f); fclose(f);
    return move_file(tmp, path);
}
static void note_path(char *out, size_t n, const char *name) { snprintf(out, n, "%s/%s", dir, name); }

// Copy one line of text starting at s (up to newline) into dst, skipping a tick-box prefix.
static void line_text(char *dst, size_t n, const char *s) {
    if (s[0] == '[' && (s[1] == ' ' || s[1] == 'x' || s[1] == 'X') && s[2] == ']' && s[3] == ' ') s += 4;
    size_t i = 0; while (s[i] && s[i] != '\n' && s[i] != '\r' && i < n - 1) { dst[i] = s[i]; i++; }
    dst[i] = 0;
}

static int by_time(const void *a, const void *b) { time_t x = ((const Note *)a)->mtime, y = ((const Note *)b)->mtime; return x < y ? 1 : x > y ? -1 : 0; }

static void scan_notes(void) {
    nnotes = 0;
    DIR *d = opendir(dir); if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && nnotes < MAXN) {
        size_t l = strlen(e->d_name);
        if (e->d_name[0] == '.' || l < 5 || strcmp(e->d_name + l - 4, ".txt") || l >= sizeof notes[0].name || strstr(e->d_name, "(phone copy)")) continue;
        Note *n = &notes[nnotes]; memset(n, 0, sizeof *n);
        snprintf(n->name, sizeof n->name, "%s", e->d_name);
        char p[300]; note_path(p, sizeof p, n->name);
        struct stat st; if (stat(p, &st)) continue;
        n->mtime = st.st_mtime;
        char *b = read_file(p, NULL); if (!b) continue;
        line_text(n->title, sizeof n->title, b);
        if (!n->title[0]) snprintf(n->title, sizeof n->title, "(empty)");
        const char *q = strchr(b, '\n');
        while (q && !n->prev[0]) { q++; line_text(n->prev, sizeof n->prev, q); q = strchr(q, '\n'); }
        for (const char *s = b; s; s = strchr(s, '\n') ? strchr(s, '\n') + 1 : NULL) {
            if (s[0] == '[' && (s[1] == ' ' || s[1] == 'x' || s[1] == 'X') && s[2] == ']' && s[3] == ' ') { n->all_items++; if (s[1] == ' ') n->open_items++; }
        }
        free(b); nnotes++;
    }
    closedir(d);
    qsort(notes, nnotes, sizeof notes[0], by_time);
}

static void scan_todos(void) {
    ntodos = 0;
    for (int i = 0; i < nnotes && ntodos < MAXT; i++) {
        if (!notes[i].open_items) continue;
        char p[300]; note_path(p, sizeof p, notes[i].name);
        char *b = read_file(p, NULL); if (!b) continue;
        for (char *s = b; s && ntodos < MAXT; s = strchr(s, '\n') ? strchr(s, '\n') + 1 : NULL) {
            if (s[0] == '[' && s[1] == ' ' && s[2] == ']' && s[3] == ' ') {
                Todo *t = &todos[ntodos++]; t->note = i; t->off = (int)(s - b);
                line_text(t->text, sizeof t->text, s);
            }
        }
        free(b);
    }
}

static void toggle_todo(const Todo *t) {
    char p[300]; note_path(p, sizeof p, notes[t->note].name);
    int len; char *b = read_file(p, &len); if (!b) return;
    if (t->off + 3 < len && b[t->off + 1] == ' ') { b[t->off + 1] = 'x'; write_file(p, b, len); }
    free(b);
}

static int save_note(const char *name, const UiEdit *e) {
    char p[300]; note_path(p, sizeof p, name);
    if (e->len == 0) { unlink(p); return 0; }                    // empty notes vanish
    return write_file(p, e->buf, e->len);
}

enum { V_LIST, V_EDIT, V_TODO };

int main(int argc, char **argv) {
    double seconds = argc > 1 ? atof(argv[1]) : 86400;
    if (argc > 2) snprintf(dir, sizeof dir, "%s", argv[2]);
    PlatInfo pi;
    if (plat_init(&pi)) return 1;
    if (plat_touch_init(&pi)) return 1;
    int W = pi.w, H = pi.h;
    mkdirs(dir);

    int key_h = W / 8, kbd_h = key_h * 5 + 6, kb_y = H - kbd_h;
    Kbd kb; kbd_init(&kb, 0, kb_y, W, kbd_h);
    Ui u; memset(&u, 0, sizeof u);
    UiEdit ed; ui_edit_init(&ed, NOTE_CAP);
    UiScroll list_sc = {0}, todo_sc = {0};
    int view = V_LIST, quit = 0, dirty = 3, confirm_del = 0;
    char cur_name[64] = "";
    double t0 = ui_now(), t_blink = t0, t_save = t0; int blink = 1;
    const int BAR = 64, ROW = 84;

    scan_notes();

    while (!quit && ui_now() - t0 < seconds) {
        double t = ui_now();
        PlatTouch tc[10];
        int n = plat_touch_poll(tc, 10);
        ui_poll(&u, tc, n);

        int keys[16], nk = 0;
        if (view == V_EDIT) {
            nk = ui_keyboard(&u, &kb, tc, n, keys, 16);
            for (int i = 0; i < nk; i++) ui_edit_key(&ed, keys[i]);
            if (nk) { dirty = 3; blink = 1; t_blink = t; }
            if (kb.dirty) { dirty = 3; kb.dirty = 0; }
            if (ed.edited && t - t_save > 5) { save_note(cur_name, &ed); ed.edited = 0; t_save = t; }
        }
        if (t - t_blink >= 0.5) { blink ^= 1; t_blink = t; if (view == V_EDIT) dirty = 3; }
        if (u.changed || u.tapped) dirty = 3;
        if (dirty <= 0) { usleep(8000); continue; }
        dirty--;

        int stride; uint32_t *px = plat_begin(&stride);
        Surf s = { px, stride, W, H, pi.rs, pi.gs, pi.bs, pi.alpha, pi.theme };
        u.s = &s; u.W = W; u.H = H;
        draw_rect(&s, 0, 0, W, H, 0x10161E);
        draw_rect(&s, 0, 0, W, BAR, 0x26303E);

        if (view == V_LIST) {
            ui_label(&u, 16, 16, 2, "NOTES", 0xFFFFFF);
            int bw = (W - 140) / 3 - 8;
            if (ui_button(&u, 130, 8, bw, BAR - 16, "+NOTE", 0x2E7D4F)) {
                snprintf(cur_name, sizeof cur_name, "n%ld.txt", (long)time(NULL));
                ui_edit_set(&ed, ""); view = V_EDIT; t_save = t; confirm_del = 0;
            }
            if (ui_button(&u, 130 + (bw + 8), 8, bw, BAR - 16, "+LIST", 0x2E7D4F)) {
                snprintf(cur_name, sizeof cur_name, "n%ld.txt", (long)time(NULL));
                ui_edit_set(&ed, "Checklist\n[ ] "); view = V_EDIT; t_save = t; confirm_del = 0;
            }
            if (ui_button(&u, 130 + 2 * (bw + 8), 8, bw, BAR - 16, "TODO", 0x34608F)) { scan_todos(); todo_sc.pos = 0; view = V_TODO; }
            int ly = BAR, lh = H - BAR - 56;
            ui_scroll(&u, &list_sc, 0, ly, W, lh, nnotes * ROW);
            for (int i = 0; i < nnotes; i++) {
                int y = ly + i * ROW - list_sc.pos;
                if (y + ROW <= ly || y >= ly + lh) continue;
                char buf[128];
                draw_rect(&s, 0, y, W, ROW - 2, 0x18202A);
                ui_fit(buf, sizeof buf, notes[i].title, (W - 40) / 16);
                ui_label(&u, 16, y + 10, 2, buf, 0xF0F0F0);
                if (notes[i].all_items) snprintf(buf, sizeof buf, "%d of %d items open", notes[i].open_items, notes[i].all_items);
                else ui_fit(buf, sizeof buf, notes[i].prev, (W - 40) / 8);
                ui_label(&u, 16, y + 50, 1, buf, 0x8899AA);
                if (u.tapped && y >= ly && y + ROW <= ly + lh + ROW && ui_inside(u.tap_x, u.tap_y, 0, y, W, ROW) && u.tap_y >= ly && u.tap_y < ly + lh) {
                    char p[300]; snprintf(cur_name, sizeof cur_name, "%s", notes[i].name); note_path(p, sizeof p, cur_name);
                    char *b = read_file(p, NULL); ui_edit_set(&ed, b ? b : ""); free(b);
                    view = V_EDIT; t_save = t; confirm_del = 0; u.tapped = 0;
                }
            }
            if (!nnotes) ui_label(&u, 16, ly + 24, 2, "No notes yet. Tap +NOTE.", 0x8899AA);
            if (ui_button(&u, 8, H - 52, W - 16, 44, "EXIT", 0x883333)) quit = 1;
        } else if (view == V_TODO) {
            ui_label(&u, 16, 16, 2, "TO-DO", 0xFFFFFF);
            if (ui_button(&u, W - 140, 8, 128, BAR - 16, "BACK", 0x34608F)) { scan_notes(); view = V_LIST; }
            int ly = BAR, lh = H - BAR, rh = 72;
            ui_scroll(&u, &todo_sc, 0, ly, W, lh, ntodos * rh);
            for (int i = 0; i < ntodos; i++) {
                int y = ly + i * rh - todo_sc.pos;
                if (y + rh <= ly || y >= ly + lh) continue;
                char buf[128];
                draw_rect(&s, 0, y, W, rh - 2, 0x18202A);
                draw_rect(&s, 16, y + 14, 36, 36, 0x8899AA); draw_rect(&s, 19, y + 17, 30, 30, 0x10161E);
                ui_fit(buf, sizeof buf, todos[i].text, (W - 80) / 16);
                ui_label(&u, 68, y + 8, 2, buf, 0xF0F0F0);
                ui_fit(buf, sizeof buf, notes[todos[i].note].title, (W - 80) / 8);
                ui_label(&u, 68, y + 46, 1, buf, 0x778899);
                if (u.tapped && u.tap_y >= ly && ui_inside(u.tap_x, u.tap_y, 0, y, 140, rh)) {
                    toggle_todo(&todos[i]); u.tapped = 0; scan_notes(); scan_todos(); dirty = 3; break;
                }
            }
            if (!ntodos) ui_label(&u, 16, ly + 24, 2, "Nothing to do.", 0x8899AA);
        } else {
            if (ui_button(&u, 8, 8, 120, BAR - 16, "BACK", 0x34608F)) {
                save_note(cur_name, &ed); ed.edited = 0; scan_notes(); view = V_LIST; dirty = 3;
            }
            if (ui_button(&u, 136, 8, 120, BAR - 16, "BOX", 0x2E7D4F)) { ui_edit_toggle_box_prefix(&ed); dirty = 3; }
            if (ui_button(&u, W - 128, 8, 120, BAR - 16, confirm_del ? "SURE?" : "DEL", 0x883333)) {
                if (confirm_del) { ed.len = 0; ed.buf[0] = 0; save_note(cur_name, &ed); ed.edited = 0; scan_notes(); view = V_LIST; confirm_del = 0; }
                else confirm_del = 1;
            } else if (u.tapped && !ui_inside(u.tap_x, u.tap_y, W - 128, 8, 120, BAR - 16)) confirm_del = 0;
            if (view == V_EDIT) {
                if (ui_edit_draw(&u, &ed, 0, BAR, W, kb_y - BAR, 2, blink)) dirty = 3;
                kbd_render(&kb, &s);
            }
        }
        plat_flip();
    }
    if (view == V_EDIT) save_note(cur_name, &ed);
    plat_close();
    return 0;
}
