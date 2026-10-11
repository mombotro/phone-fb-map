// player.c - touch music player on the iOS framebuffer. Browses a folder tree, plays MP3, M4A/AAC, FLAC, Ogg, WAV.
// usage: musicplayer [music_dir] [seconds]     (defaults: /var/root/music, 86400)
// Top: folder list (drag to scroll, tap to open or play). Bottom: title, time, seek bar, prev/play/next, volume.
#include "../common/plat.h"
#include "../common/draw.h"
#include <dirent.h>
#ifdef __APPLE__
#include <execinfo.h>
#endif
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#define PATH_MAX_LEN 1024

static double now(void) { struct timeval tv; gettimeofday(&tv, NULL); return tv.tv_sec + tv.tv_usec / 1e6; }

static double g_start;
#define LOG(...) do { fprintf(stderr, "[%7.2f] ", now() - g_start); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); fflush(stderr); } while (0)

// A crash writes the signal and a backtrace to the log (stderr), then exits.
static void crash_handler(int sig) {
    void *bt[40];
    fprintf(stderr, "\n*** caught signal %d ***\n", sig); fflush(stderr);
    plat_emergency_cleanup();
#ifdef __APPLE__
    int n = backtrace(bt, 40);
    backtrace_symbols_fd(bt, n, fileno(stderr));
#else
    (void)bt;
#endif
    _exit(128 + sig);
}

// ---------------------------------------------------------------- audio engine
// Two backends with the same functions: audio_ios.h (decoders + AudioQueue) and audio_android.h (a Java helper that
// plays with the platform MediaPlayer). Both read g_volume and set g_playing, g_paused and g_eof.
static float g_volume = 0.7f;
static int g_paused, g_playing;
static volatile int g_eof;                // the file reached its end
#ifdef __APPLE__
#include "audio_ios.h"
#else
#include "audio_android.h"
#endif

// ---------------------------------------------------------------- library

typedef struct { char name[256]; int is_dir; } Entry;

static char g_root[PATH_MAX_LEN], g_cwd[PATH_MAX_LEN];
static int g_scroll;
static Entry *g_ents; static int g_nent;
static char **g_pl; static int g_pl_n, g_pl_i = -1;           // the playlist = files of the folder we started from
static char g_pl_dir[PATH_MAX_LEN];

static int cmp_entry(const void *a, const void *b) {
    const Entry *x = a, *y = b;
    if (x->is_dir != y->is_dir) return y->is_dir - x->is_dir;                  // folders first
    return strcasecmp(x->name, y->name);
}

static void scan_dir(void) {
    free(g_ents); g_ents = NULL; g_nent = 0;
    int cap = 64;
    g_ents = malloc(cap * sizeof(Entry));
    if (strcmp(g_cwd, g_root) != 0) { snprintf(g_ents[g_nent].name, 256, ".."); g_ents[g_nent++].is_dir = 1; }
    DIR *d = opendir(g_cwd);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.') continue;
            char full[PATH_MAX_LEN * 2]; snprintf(full, sizeof full, "%s/%s", g_cwd, e->d_name);
            struct stat st;
            if (stat(full, &st)) continue;
            int isd = S_ISDIR(st.st_mode);
            if (!isd && !dec_supported(e->d_name)) continue;
            if (g_nent == cap) { cap *= 2; g_ents = realloc(g_ents, cap * sizeof(Entry)); }
            snprintf(g_ents[g_nent].name, 256, "%s", e->d_name);
            g_ents[g_nent++].is_dir = isd;
        }
        closedir(d);
    }
    int first = (g_nent > 0 && !strcmp(g_ents[0].name, "..")) ? 1 : 0;
    qsort(g_ents + first, g_nent - first, sizeof(Entry), cmp_entry);
}

static void free_playlist(void) {
    for (int i = 0; i < g_pl_n; i++) free(g_pl[i]);
    free(g_pl); g_pl = NULL; g_pl_n = 0; g_pl_i = -1;
}

// Make the current folder's files the playlist and start the one at entry index `idx`.
static int play_entry(int idx) {
    free_playlist();
    g_pl = malloc((g_nent + 1) * sizeof(char *));
    int chosen = -1;
    for (int i = 0; i < g_nent; i++) {
        if (g_ents[i].is_dir) continue;
        char full[PATH_MAX_LEN * 2]; snprintf(full, sizeof full, "%s/%s", g_cwd, g_ents[i].name);
        if (i == idx) chosen = g_pl_n;
        g_pl[g_pl_n++] = strdup(full);
    }
    snprintf(g_pl_dir, sizeof g_pl_dir, "%s", g_cwd);
    g_pl_i = chosen;
    return chosen >= 0 ? play_file(g_pl[chosen]) : -1;
}

static int play_step(int dir) {                    // move through the playlist; -1 when the list ends
    int i = g_pl_i + dir;
    while (i >= 0 && i < g_pl_n) {
        if (!play_file(g_pl[i])) { g_pl_i = i; return 0; }
        i += dir;                                  // skip files that fail to open
    }
    return -1;
}

static const char *base_name(const char *p) { const char *s = strrchr(p, '/'); return s ? s + 1 : p; }

// A file named .play in the music folder asks the player to play one file. Its content is a single absolute
// path inside the music folder. The Mac app "Send to Phone" writes it after it copies a dropped file.
// The player reads the request, deletes the file, then opens the folder of the file and plays it.
static int play_path(const char *path, int list_h, int row_h) {
    char dir[PATH_MAX_LEN];
    snprintf(dir, sizeof dir, "%s", path);
    char *sl = strrchr(dir, '/');
    if (!sl) return -1;
    const char *file = sl + 1;
    *sl = 0;
    size_t rl = strlen(g_root);
    if (strncmp(dir, g_root, rl) != 0 || (dir[rl] != 0 && dir[rl] != '/')) return -1;   // only inside the music folder
    snprintf(g_cwd, sizeof g_cwd, "%s", dir);
    scan_dir();
    int idx = -1;
    for (int i = 0; i < g_nent; i++) if (!g_ents[i].is_dir && !strcmp(g_ents[i].name, file)) idx = i;
    if (idx < 0) return -1;
    g_scroll = idx * row_h - list_h / 2 + row_h / 2;                                    // show the file in the middle
    if (g_scroll < 0) g_scroll = 0;
    return play_entry(idx);
}

static int read_play_request(char *out, size_t n) {
    char req[PATH_MAX_LEN + 16];
    snprintf(req, sizeof req, "%s/.play", g_root);
    FILE *f = fopen(req, "r");
    if (!f) return 0;
    out[0] = 0;
    if (fgets(out, (int)n, f)) { size_t l = strlen(out); while (l && (out[l - 1] == '\n' || out[l - 1] == '\r')) out[--l] = 0; }
    fclose(f);
    unlink(req);
    return out[0] != 0;
}

// ---------------------------------------------------------------- user interface

static void fit(char *dst, size_t n, const char *src, int cols) {      // copy, cut to `cols`, drop the extension
    snprintf(dst, n, "%s", src);
    char *dot = strrchr(dst, '.');
    if (dot && dot != dst && strlen(dot) <= 6) *dot = 0;
    if ((int)strlen(dst) > cols) { dst[cols - 1] = '~'; dst[cols] = 0; }
}

static void fmt_time(char *dst, size_t n, double s) {
    int t = (int)s; snprintf(dst, n, "%d:%02d", t / 60, t % 60);
}

int main(int argc, char **argv) {
    g_start = now();
    signal(SIGSEGV, crash_handler); signal(SIGBUS, crash_handler); signal(SIGABRT, crash_handler);
    signal(SIGILL, crash_handler); signal(SIGFPE, crash_handler); signal(SIGTERM, crash_handler);
    snprintf(g_root, sizeof g_root, "%s", argc > 1 ? argv[1] : "/var/root/music");
    double seconds = argc > 2 ? atof(argv[2]) : 86400;
    mkdir(g_root, 0755);
    snprintf(g_cwd, sizeof g_cwd, "%s", g_root);

    if (audio_load()) { fprintf(stderr, "cannot start the audio engine\n"); return 1; }
    PlatInfo pi;
    if (plat_init(&pi) || plat_touch_init(&pi)) return 1;
    const int W = pi.w, H = pi.h;

    const int HDR = 64, ROW = 62, PANEL_Y = H - 400;
    const int LIST_Y0 = HDR, LIST_Y1 = PANEL_Y, LIST_H = LIST_Y1 - LIST_Y0;
    const int BAR_Y = PANEL_Y + 112, BAR_H = 40, BAR_X0 = 20, BAR_X1 = W - 20;
    const int CTL_Y = PANEL_Y + 180, CTL_H = 100, VOL_Y = PANEL_Y + 300, VOL_H = 80;
    const int COLS = (W - 24) / 16;

    scan_dir();
    int dirty = 3;
    double last_sec = -1, eof_t = 0, t0 = now();

    // per-touch gesture state
    int prev[16] = {0}, zone[16] = {0}, sy[16], ly[16], moved[16] = {0}, sx_[16];
    double down_t[16] = {0};
    enum { Z_NONE, Z_LIST, Z_BAR, Z_BTN };
    int pressed_btn = -1;

    double next_req = 0;
    while (now() - t0 < seconds) {
        double t = now();
        if (t >= next_req) {                                          // poll for a play request twice a second
            next_req = t + 0.5;
            char req[PATH_MAX_LEN];
            if (read_play_request(req, sizeof req)) {
                if (play_path(req, LIST_H, ROW)) fprintf(stderr, "cannot play requested file %s\n", req);
                dirty = 3;
            }
        }
        audio_poll();
        PlatTouch tc[10];
        int n = plat_touch_poll(tc, 10);
        int cur[16] = {0};
        int quit = 0;

        for (int i = 0; i < n; i++) {
            int id = tc[i].id & 15, x = tc[i].x, y = tc[i].y;
            cur[id] = 1;
            if (!prev[id]) {                                          // touch-down
                sy[id] = ly[id] = y; sx_[id] = x; moved[id] = 0; down_t[id] = t; zone[id] = Z_NONE;
                if (y < HDR) { if (x > W - 130) quit = 1; }
                else if (y < LIST_Y1) zone[id] = Z_LIST;
                else if (y >= BAR_Y - 10 && y < BAR_Y + BAR_H + 10 && g_playing && audio_duration() > 0) {
                    zone[id] = Z_BAR;
                    double f = (double)(x - BAR_X0) / (BAR_X1 - BAR_X0); if (f < 0) f = 0; if (f > 1) f = 1;
                    audio_seek_fraction(f);
                    dirty = 3;
                } else if (y >= CTL_Y && y < CTL_Y + CTL_H) {
                    zone[id] = Z_BTN;
                    if (x < W / 3) {                                  // previous: restart if more than 3 s in
                        if (g_playing && position_seconds() > 3) audio_seek_fraction(0);
                        else if (g_pl_n) play_step(-1);
                        pressed_btn = 0;
                    } else if (x < 2 * W / 3) {                       // play / pause
                        if (g_playing) audio_set_paused(!g_paused);
                        else if (g_pl_n && g_pl_i >= 0) play_step(0);
                        pressed_btn = 1;
                    } else { if (g_pl_n) play_step(1); pressed_btn = 2; }
                    dirty = 3;
                } else if (y >= VOL_Y && y < VOL_Y + VOL_H) {
                    zone[id] = Z_BTN;
                    if (x < W / 3) g_volume -= 0.1f; else if (x >= 2 * W / 3) g_volume += 0.1f;
                    if (g_volume < 0) g_volume = 0; if (g_volume > 1) g_volume = 1;
                    audio_set_volume(g_volume);
                    pressed_btn = x < W / 3 ? 3 : (x >= 2 * W / 3 ? 4 : -1);
                    dirty = 3;
                }
            } else if (zone[id] == Z_LIST) {                          // drag scroll
                if (abs(y - sy[id]) > 14) moved[id] = 1;
                if (moved[id]) { g_scroll -= y - ly[id]; dirty = 3; }
                ly[id] = y;
            } else if (zone[id] == Z_BAR && g_playing && audio_duration() > 0 && abs(x - sx_[id]) > 6) {
                double f = (double)(x - BAR_X0) / (BAR_X1 - BAR_X0); if (f < 0) f = 0; if (f > 1) f = 1;
                audio_seek_fraction(f); sx_[id] = x; dirty = 3;
            }
        }
        for (int id = 0; id < 16; id++) {
            if (prev[id] && !cur[id]) {                               // touch-up
                if (zone[id] == Z_LIST && !moved[id] && t - down_t[id] < 0.7) {
                    int row = (sy[id] - LIST_Y0 + g_scroll) / ROW;
                    if (row >= 0 && row < g_nent) {
                        if (g_ents[row].is_dir) {
                            if (!strcmp(g_ents[row].name, "..")) { char *sl = strrchr(g_cwd, '/'); if (sl && sl != g_cwd) *sl = 0; }
                            else { size_t l = strlen(g_cwd); snprintf(g_cwd + l, sizeof g_cwd - l, "/%s", g_ents[row].name); }
                            scan_dir(); g_scroll = 0;
                        } else if (play_entry(row)) fprintf(stderr, "cannot play %s\n", g_ents[row].name);
                        dirty = 3;
                    }
                }
                zone[id] = Z_NONE; pressed_btn = -1; dirty = 3;
            }
        }
        for (int id = 0; id < 16; id++) prev[id] = cur[id];
        if (quit) break;

        int max_scroll = g_nent * ROW - LIST_H; if (max_scroll < 0) max_scroll = 0;
        if (g_scroll < 0) g_scroll = 0; if (g_scroll > max_scroll) g_scroll = max_scroll;

        // end of file: wait for the queued audio to play out, then go to the next file
        if (g_playing && g_eof) {
            if (eof_t == 0) { eof_t = t; char info[160]; audio_end_info(info, sizeof info); LOG("reached end: %s", info); }
            else if (t - eof_t > 0.45) { eof_t = 0; if (play_step(1)) stop_audio(); dirty = 3; }
        } else eof_t = 0;

        double sec = g_playing ? position_seconds() : -1;
        static double next_beat = 0;
        if (t >= next_beat) { next_beat = t + 5; char info[160]; audio_info(info, sizeof info); LOG("heartbeat: playing=%d paused=%d pos=%.1fs %s", g_playing, g_paused, sec, info); }
        if ((int)sec != (int)last_sec) { last_sec = sec; dirty = 3; }

        if (dirty > 0) {
            int stride; uint32_t *px = plat_begin(&stride);
            Surf s = { px, stride, W, H, pi.rs, pi.gs, pi.bs, pi.alpha, pi.theme };
            draw_rect(&s, 0, 0, W, H, 0x101820);

            // list (clipped to its area by drawing rows, then covering the overflow with the header and panel)
            for (int r = 0; r < g_nent; r++) {
                int y = LIST_Y0 + r * ROW - g_scroll;
                if (y + ROW <= LIST_Y0 || y >= LIST_Y1) continue;
                int is_cur = g_playing && !g_ents[r].is_dir && !strcmp(g_cwd, g_pl_dir) && g_pl_i >= 0 &&
                             !strcmp(g_ents[r].name, base_name(g_pl[g_pl_i]));
                if (is_cur) draw_rect(&s, 0, y, W, ROW - 2, 0x1F3A2A);
                char line[300]; fit(line, sizeof line, g_ents[r].name, COLS - 1);
                if (g_ents[r].is_dir && strcmp(g_ents[r].name, "..")) strncat(line, "/", sizeof line - strlen(line) - 1);
                draw_text(&s, 12, y + 15, 2, line, g_ents[r].is_dir ? 0x77CCFF : (is_cur ? 0x77FF99 : 0xFFFFFF));
                draw_rect(&s, 0, y + ROW - 2, W, 1, 0x1E2733);
            }
            if (g_nent == 0 || (g_nent == 1 && !strcmp(g_ents[0].name, "..")))
                draw_text(&s, 12, LIST_Y0 + 20, 2, "No music here.", 0x888888);
            if (g_nent == 0) draw_text(&s, 12, LIST_Y0 + 70, 1, "Copy files to /var/root/music with sync_music.py", 0x888888);

            draw_rect(&s, 0, 0, W, HDR, 0x1B2330);
            char path[300]; snprintf(path, sizeof path, "%s", g_cwd + strlen(g_root));
            if (!path[0]) snprintf(path, sizeof path, "/");
            int pl = (int)strlen(path); const char *pp = path;
            if (pl > COLS - 9) pp = path + pl - (COLS - 9);
            draw_text(&s, 12, 16, 2, pp, 0xCCCCCC);
            draw_rect(&s, W - 130, 0, 130, HDR, 0x883333);
            draw_text(&s, W - 112, 16, 2, "EXIT", 0xFFFFFF);

            draw_rect(&s, 0, PANEL_Y, W, H - PANEL_Y, 0x1B2330);
            char title[300] = "Nothing playing";
            if (g_pl_i >= 0 && g_playing) fit(title, sizeof title, base_name(g_pl[g_pl_i]), COLS);
            draw_text(&s, 12, PANEL_Y + 16, 2, title, 0xFFFFFF);
            char tcur[16] = "0:00", ttot[16] = "0:00", tl[48];
            if (g_playing) fmt_time(tcur, sizeof tcur, sec);
            if (g_playing && audio_duration() > 0) fmt_time(ttot, sizeof ttot, audio_duration());
            snprintf(tl, sizeof tl, "%s / %s%s", tcur, ttot, g_paused ? "  (paused)" : "");
            draw_text(&s, 12, PANEL_Y + 62, 2, tl, 0x99AABB);

            draw_rect(&s, BAR_X0, BAR_Y, BAR_X1 - BAR_X0, BAR_H, 0x2A3140);
            if (g_playing && audio_duration() > 0) {
                double f = audio_fraction();
                draw_rect(&s, BAR_X0, BAR_Y, (int)((BAR_X1 - BAR_X0) * f), BAR_H, 0x4C8DDB);
            }

            static const char *bl[3] = { "<<", "", ">>" };
            for (int b = 0; b < 3; b++) {
                int x0 = b * W / 3 + 8, w = W / 3 - 16;
                draw_rect(&s, x0, CTL_Y, w, CTL_H, pressed_btn == b ? 0x5A80B8 : 0x2A3850);
                const char *lab = b == 1 ? (g_playing && !g_paused ? "PAUSE" : "PLAY") : bl[b];
                draw_text(&s, x0 + (w - (int)strlen(lab) * 32) / 2, CTL_Y + (CTL_H - 32) / 2, 2 * 1, lab, 0xFFFFFF);
            }
            draw_rect(&s, 8, VOL_Y, W / 3 - 16, VOL_H, pressed_btn == 3 ? 0x5A80B8 : 0x2A3850);
            draw_text(&s, 8 + (W / 3 - 16 - 48) / 2, VOL_Y + 24, 2, "VOL-", 0xFFFFFF);
            draw_rect(&s, 2 * W / 3 + 8, VOL_Y, W / 3 - 16, VOL_H, pressed_btn == 4 ? 0x5A80B8 : 0x2A3850);
            draw_text(&s, 2 * W / 3 + 8 + (W / 3 - 16 - 48) / 2, VOL_Y + 24, 2, "VOL+", 0xFFFFFF);
            int vx0 = W / 3 + 8, vw = W / 3 - 16;
            draw_rect(&s, vx0, VOL_Y + VOL_H / 2 - 10, vw, 20, 0x2A3140);
            draw_rect(&s, vx0, VOL_Y + VOL_H / 2 - 10, (int)(vw * g_volume), 20, 0x77CC88);

            plat_flip();
            dirty--;
        } else {
            usleep(12000);
        }
    }
    LOG("exit: normal");
    stop_audio();
#ifndef __APPLE__
    audio_shutdown();
#endif
    plat_close();
    return 0;
}
