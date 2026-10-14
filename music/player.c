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
#include <fcntl.h>
#ifndef __APPLE__
#include "../common/musicstate.h"
#endif

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
static float g_volume = 1.0f;            // the player adds nothing: the device volume is the volume (Android: the master volume file, iOS: the buttons)
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
static int g_shuffle, g_repeat;                                 // g_repeat: 0 off, 1 the folder, 2 the song
static unsigned char *g_done;                                   // shuffle: which files of the folder already played
static int g_hist[256], g_nhist;                                // shuffle: the files played before, for the previous button

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
    free(g_pl); g_pl = NULL; g_pl_n = 0; g_pl_i = -1; free(g_done); g_done = NULL; g_nhist = 0;
}

// Make the current folder's files the playlist and start the one at entry index `idx`.
static int eng_play_entry(int idx) {
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
    g_done = calloc((size_t)g_pl_n + 1, 1); g_nhist = 0;
    if (chosen >= 0 && g_done) g_done[chosen] = 1;
    return chosen >= 0 ? play_file(g_pl[chosen]) : -1;
}

static int play_index(int i) {
    if (play_file(g_pl[i])) return -1;
    g_pl_i = i; if (g_done) g_done[i] = 1;
    return 0;
}

// A random file that did not play yet. When all played, repeat "folder" starts a new round, else it is over (-1).
static int pick_random(void) {
    for (int round = 0; round < 2; round++) {
        int left = 0; for (int i = 0; i < g_pl_n; i++) if (!g_done[i]) left++;
        if (left) { int k = rand() % left; for (int i = 0; i < g_pl_n; i++) if (!g_done[i] && k-- == 0) return i; }
        if (g_repeat != 1) return -1;
        memset(g_done, 0, (size_t)g_pl_n); if (g_pl_i >= 0) g_done[g_pl_i] = 1;      // new round: all but this one
    }
    return -1;
}

// Move through the playlist: dir 1 next, -1 previous, 0 this file again. -1 when there is nowhere to go.
static int play_step(int dir) {
    if (g_pl_n <= 0) return -1;
    if (dir == 0) return play_index(g_pl_i < 0 ? 0 : g_pl_i);
    if (g_shuffle && g_done) {
        if (dir < 0) {                                                  // back through what played
            while (g_nhist > 0) { int i = g_hist[--g_nhist]; if (!play_index(i)) return 0; }
            return -1;
        }
        for (int tries = 0; tries < g_pl_n; tries++) {                  // skip files that fail to open
            int i = pick_random(); if (i < 0) return -1;
            int prev = g_pl_i;
            if (!play_index(i)) { if (prev >= 0 && g_nhist < 256) g_hist[g_nhist++] = prev; return 0; }
            g_done[i] = 1;
        }
        return -1;
    }
    int i = g_pl_i + dir, wrapped = 0;
    for (int tries = 0; tries < g_pl_n + 1; tries++) {
        if ((i < 0 || i >= g_pl_n)) {
            if (g_repeat != 1 || wrapped) return -1;                    // repeat folder: round to the other end once
            i = dir > 0 ? 0 : g_pl_n - 1; wrapped = 1;
        }
        if (!play_index(i)) return 0;
        i += dir;                                                       // skip files that fail to open
    }
    return -1;
}

// The file ended by itself.
static int play_after_end(void) { return g_repeat == 2 ? play_step(0) : play_step(1); }

static void shuffle_set(int on) {
    g_shuffle = on; g_nhist = 0;
    if (g_done) { memset(g_done, 0, (size_t)g_pl_n); if (g_pl_i >= 0) g_done[g_pl_i] = 1; }
}

// The modes are kept in a file in the music folder, so they stay after the player closes.
static void modes_file(char *p, size_t n) { snprintf(p, n, "%s/.modes", g_root); }
static void modes_load(void) {
    char p[PATH_MAX_LEN + 16]; modes_file(p, sizeof p);
    FILE *f = fopen(p, "r"); if (!f) return;
    int a = 0, b = 0; if (fscanf(f, "%d %d", &a, &b) == 2) { g_shuffle = a != 0; g_repeat = b < 0 || b > 2 ? 0 : b; }
    fclose(f);
}
static void modes_save(void) {
    char p[PATH_MAX_LEN + 16]; modes_file(p, sizeof p);
    FILE *f = fopen(p, "w"); if (!f) return;
    fprintf(f, "%d %d\n", g_shuffle, g_repeat); fclose(f);
}

static const char *base_name(const char *p) { const char *s = strrchr(p, '/'); return s ? s + 1 : p; }

// ---- the player screen and the audio. On iOS the screen plays the audio itself. On Android the audio and the playlist live in a
// background process (the "daemon", below) that goes on when this screen closes. The screen sends it commands and shows its state.
#ifdef __APPLE__
static const char *cur_dir(void) { return g_pl_dir; }
static const char *cur_file(void) { return g_pl_i >= 0 ? base_name(g_pl[g_pl_i]) : ""; }
static int play_entry(int idx) { return eng_play_entry(idx); }
#define U_PREV() do { if (g_playing && position_seconds() > 3) audio_seek_fraction(0); else if (g_pl_n) play_step(-1); } while (0)
#define U_TOGGLE() do { if (g_playing) audio_set_paused(!g_paused); else if (g_pl_n && g_pl_i >= 0) play_step(0); } while (0)
#define U_NEXT() do { if (g_pl_n) play_step(1); } while (0)
#define U_SEEK(f) audio_seek_fraction(f)
#define U_SHUFFLE(on) do { shuffle_set(on); modes_save(); } while (0)
#define U_REPEAT(m) do { g_repeat = (m); modes_save(); } while (0)
#else
static char g_ms_dir[1024], g_ms_file[256];
static const char *cur_dir(void) { return g_ms_dir; }
static const char *cur_file(void) { return g_ms_file; }
static int play_entry(int idx) { music_cmd("play %s\t%s", g_cwd, g_ents[idx].name); return 0; }
#define U_PREV() music_cmd("prev")
#define U_TOGGLE() do { music_cmd(g_playing ? (g_paused ? "resume" : "pause") : "replay"); if (g_playing) g_paused = !g_paused; } while (0)
#define U_NEXT() music_cmd("next")
#define U_SEEK(f) do { audio_seek_fraction(f); music_cmd("seek %.4f", (double)(f)); } while (0)
#define U_SHUFFLE(on) do { g_shuffle = (on); music_cmd("shuffle %d", g_shuffle); } while (0)
#define U_REPEAT(m) do { g_repeat = (m); music_cmd("repeat %d", g_repeat); } while (0)
// Take over what the background process says. Returns 1 when something that shows on the screen changed.
static int ui_sync(void) {
    MusicState m; music_state_read(&m);
    int ch = m.playing != g_playing || m.paused != g_paused || m.shuffle != g_shuffle || m.repeat != g_repeat ||
             strcmp(m.file, g_ms_file) || strcmp(m.dir, g_ms_dir);
    g_playing = m.playing; g_paused = m.paused; g_shuffle = m.shuffle; g_repeat = m.repeat;
    g_pos_ms = m.pos_ms; g_dur_ms = m.dur_ms;
    snprintf(g_ms_dir, sizeof g_ms_dir, "%s", m.dir); snprintf(g_ms_file, sizeof g_ms_file, "%s", m.file);
    return ch;
}
// Start the background process if it does not run yet (before the display opens: starting a process later turns some screens black).
static void ensure_daemon(void) {
    MusicState m; if (music_state_read(&m)) return;
    char self[256]; ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1); if (n <= 0) return;
    self[n] = 0; { char *dl = strstr(self, " (deleted)"); if (dl) *dl = 0; }
    pid_t p = fork();
    if (p == 0) { execl(self, self, "--daemon", g_root, (char *)NULL); _exit(127); }
    for (int i = 0; i < 200 && p > 0; i++) { usleep(50000); if (music_state_read(&m)) break; }
}
#endif


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

#ifndef __APPLE__
// ---- the background process (musicplayer --daemon ROOT): plays the music, moves on to the next file, keeps shuffle and repeat,
// and answers the commands in MUSIC_CMD. It writes MUSIC_STATE four times a second. It ends on "quit", or ten minutes after the music stopped.
static int daemon_command(char *l) {                    // 1 = quit
    l[strcspn(l, "\r\n")] = 0;
    char *arg = strchr(l, ' '); if (arg) *arg++ = 0; else arg = l + strlen(l);
    LOG("command: %s %s", l, arg);
    if (!strcmp(l, "quit")) return 1;
    if (!strcmp(l, "play")) {                            // arg: folder TAB file
        char *tab = strchr(arg, '\t'); if (!tab) return 0;
        *tab = 0; const char *dir = arg, *file = tab + 1; size_t rl = strlen(g_root);
        if (strncmp(dir, g_root, rl) || (dir[rl] && dir[rl] != '/')) return 0;       // only inside the music folder
        snprintf(g_cwd, sizeof g_cwd, "%s", dir); scan_dir();
        for (int i = 0; i < g_nent; i++) if (!g_ents[i].is_dir && !strcmp(g_ents[i].name, file)) { if (eng_play_entry(i)) LOG("cannot play %s", file); break; }
    }
    else if (!strcmp(l, "pause")) { if (g_playing) audio_set_paused(1); }
    else if (!strcmp(l, "resume")) { if (g_playing) audio_set_paused(0); }
    else if (!strcmp(l, "replay")) { if (!g_playing && g_pl_i >= 0) play_step(0); }
    else if (!strcmp(l, "next")) { if (g_pl_n) play_step(1); }
    else if (!strcmp(l, "prev")) { if (g_playing && position_seconds() > 3) audio_seek_fraction(0); else if (g_pl_n) play_step(-1); }
    else if (!strcmp(l, "seek")) audio_seek_fraction(atof(arg));
    else if (!strcmp(l, "shuffle")) { shuffle_set(atoi(arg) != 0); modes_save(); }
    else if (!strcmp(l, "repeat")) { int m = atoi(arg); g_repeat = m < 0 || m > 2 ? 0 : m; modes_save(); }
    else if (!strcmp(l, "stop")) { stop_audio(); free_playlist(); }
    return 0;
}

static int daemon_main(const char *root) {
    g_start = now();
    setsid(); signal(SIGHUP, SIG_IGN); signal(SIGPIPE, SIG_IGN);
    int fd = open("/data/local/tmp/musicd.out", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); if (fd > 2) close(fd); }
    int nul = open("/dev/null", O_RDONLY); if (nul >= 0) { dup2(nul, 0); if (nul > 2) close(nul); }
    snprintf(g_root, sizeof g_root, "%s", root); snprintf(g_cwd, sizeof g_cwd, "%s", root);
    mkdir(g_root, 0755);
    modes_load();
    if (audio_load()) { LOG("cannot start the audio engine"); return 1; }
    unlink(MUSIC_CMD);
    off_t cmd_off = 0;
    double eof_t = 0, next_state = 0, next_vol = 0, idle0 = now();
    int master = -1, quit = 0, fails = 0;
    while (!quit) {
        double t = now();
        audio_poll();
        {                                                               // new lines in the command file (it is only ever added to; no rename: some phones deny it)
            struct stat st;
            if (stat(MUSIC_CMD, &st) == 0) {
                if (st.st_size < cmd_off) cmd_off = 0;
                if (st.st_size > cmd_off) {
                    FILE *f = fopen(MUSIC_CMD, "r");
                    if (f) {
                        fseek(f, cmd_off, SEEK_SET); char l[1200];
                        while (!quit && ftell(f) < st.st_size && fgets(l, sizeof l, f)) quit = daemon_command(l);
                        cmd_off = st.st_size; fclose(f);
                    }
                } else if (st.st_size > 4096) { if (truncate(MUSIC_CMD, 0) == 0) cmd_off = 0; }
            } else cmd_off = 0;
        }
        if (g_playing && g_eof) {                                       // the file ended: let the queued sound play out, then go on
            if (eof_t == 0) eof_t = t;
            else if (t - eof_t > 0.45) {
                eof_t = 0;
                if (g_err_eof) {                                        // the helper could not play the file: do not go through the whole list at once
                    g_err_eof = 0; fails++;
                    if (fails == 2) { LOG("the helper fails: starting it again"); audio_shutdown(); if (audio_load()) { stop_audio(); fails = 0; } else if (play_step(0)) stop_audio(); }
                    else if (fails >= 4) { LOG("four files in a row failed: stopping"); stop_audio(); fails = 0; }
                    else if (play_after_end()) stop_audio();
                } else { fails = 0; if (play_after_end()) stop_audio(); }
            }
        } else eof_t = 0;
        if (t >= next_vol) { next_vol = t + 0.5; int m = plat_volume_get(); if (m != master) { master = m; audio_set_volume(1); } }
        if (t >= next_state) {
            next_state = t + 0.25;
            MusicState ms; memset(&ms, 0, sizeof ms);
            ms.playing = g_playing; ms.paused = g_paused; ms.pos_ms = g_pos_ms; ms.dur_ms = g_dur_ms; ms.shuffle = g_shuffle; ms.repeat = g_repeat;
            snprintf(ms.dir, sizeof ms.dir, "%s", g_pl_dir); snprintf(ms.file, sizeof ms.file, "%s", g_pl_i >= 0 ? base_name(g_pl[g_pl_i]) : "");
            music_state_write(&ms);
        }
        if (g_playing) idle0 = t; else if (t - idle0 > 600) { LOG("idle for ten minutes: ends"); break; }
        usleep(50000);
    }
    unlink(MUSIC_STATE);
    stop_audio(); audio_shutdown();
    return 0;
}
#endif

int main(int argc, char **argv) {
#ifndef __APPLE__
    if (argc > 2 && !strcmp(argv[1], "--daemon")) return daemon_main(argv[2]);
#endif
    g_start = now(); srand((unsigned)(g_start * 1000));
    signal(SIGSEGV, crash_handler); signal(SIGBUS, crash_handler); signal(SIGABRT, crash_handler);
    signal(SIGILL, crash_handler); signal(SIGFPE, crash_handler); signal(SIGTERM, crash_handler);
    snprintf(g_root, sizeof g_root, "%s", argc > 1 ? argv[1] : "/var/root/music");
    double seconds = argc > 2 ? atof(argv[2]) : 86400;
    mkdir(g_root, 0755);
    snprintf(g_cwd, sizeof g_cwd, "%s", g_root);

#ifdef __APPLE__
    if (audio_load()) { fprintf(stderr, "cannot start the audio engine\n"); return 1; }
#else
    ensure_daemon();
#endif
    PlatInfo pi;
    if (plat_init(&pi) || plat_touch_init(&pi)) return 1;
    const int W = pi.w, H = pi.h;

    const int HDR = 64, ROW = 62, PANEL_Y = H - 480;
    const int LIST_Y0 = HDR, LIST_Y1 = PANEL_Y, LIST_H = LIST_Y1 - LIST_Y0;
    const int BAR_Y = PANEL_Y + 112, BAR_H = 40, BAR_X0 = 20, BAR_X1 = W - 20;
    const int CTL_Y = PANEL_Y + 180, CTL_H = 100, MODE_Y = PANEL_Y + 296, MODE_H = 66, VOL_Y = PANEL_Y + 380, VOL_H = 80;
    const int VOL_X0 = 20, VOL_X1 = W - 20; (void)VOL_X1;
    const int COLS = (W - 24) / 16;

    modes_load();
    scan_dir();
    int dirty = 3;
    double last_sec = -1, eof_t = 0, t0 = now(); (void)eof_t;

    // per-touch gesture state
    int prev[16] = {0}, zone[16] = {0}, sy[16], ly[16], moved[16] = {0}, sx_[16];
    double down_t[16] = {0};
    enum { Z_NONE, Z_LIST, Z_BAR, Z_BTN, Z_VOL };
#ifndef __APPLE__
    int master = plat_volume_get(); double next_vol = 0;
    audio_set_volume(1);
#endif
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
#ifndef __APPLE__
        if (plat_volume_event() || t >= next_vol) {                    // the volume keys, or another program, changed the master volume
            next_vol = t + 0.5;
            int m = plat_volume_get(); if (m != master) { master = m; audio_set_volume(1); dirty = 3; }
        }
#endif
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
                    U_SEEK(f);
                    dirty = 3;
                } else if (y >= CTL_Y && y < CTL_Y + CTL_H) {
                    zone[id] = Z_BTN;
                    if (x < W / 3) { U_PREV(); pressed_btn = 0; }       // previous: restart if more than 3 s in
                    else if (x < 2 * W / 3) { U_TOGGLE(); pressed_btn = 1; }       // play / pause
                    else { U_NEXT(); pressed_btn = 2; }
                    dirty = 3;
                } else if (y >= MODE_Y && y < MODE_Y + MODE_H) {
                    zone[id] = Z_BTN;
                    if (x < W / 2) { U_SHUFFLE(!g_shuffle); pressed_btn = 5; }
                    else { U_REPEAT((g_repeat + 1) % 3); pressed_btn = 6; }
                    dirty = 3;
                } else if (y >= VOL_Y && y < VOL_Y + VOL_H) {
                    zone[id] = Z_VOL;
#ifndef __APPLE__
                    int m = (int)((long)(x - VOL_X0) * 100 / (VOL_X1 - VOL_X0)); m = m < 0 ? 0 : m > 100 ? 100 : m;
                    master = m; plat_volume_set(m); audio_set_volume(1);
#endif
                    dirty = 3;
                }
            } else if (zone[id] == Z_LIST) {                          // drag scroll
                if (abs(y - sy[id]) > 14) moved[id] = 1;
                if (moved[id]) { g_scroll -= y - ly[id]; dirty = 3; }
                ly[id] = y;
            } else if (zone[id] == Z_VOL) {                          // drag the volume
#ifndef __APPLE__
                int m = (int)((long)(x - VOL_X0) * 100 / (VOL_X1 - VOL_X0)); m = m < 0 ? 0 : m > 100 ? 100 : m;
                if (m != master) { master = m; plat_volume_set(m); audio_set_volume(1); dirty = 3; }
#endif
            } else if (zone[id] == Z_BAR && g_playing && audio_duration() > 0 && abs(x - sx_[id]) > 6) {
                double f = (double)(x - BAR_X0) / (BAR_X1 - BAR_X0); if (f < 0) f = 0; if (f > 1) f = 1;
                U_SEEK(f); sx_[id] = x; dirty = 3;
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
#ifdef __APPLE__
        if (g_playing && g_eof) {
            if (eof_t == 0) { eof_t = t; char info[160]; audio_end_info(info, sizeof info); LOG("reached end: %s", info); }
            else if (t - eof_t > 0.45) { eof_t = 0; if (play_after_end()) stop_audio(); dirty = 3; }
        } else eof_t = 0;
#else
        { static double next_sync = 0; if (t >= next_sync) { next_sync = t + 0.2; if (ui_sync()) dirty = 3; } }       // the background process plays and moves on by itself
#endif

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
                int is_cur = g_playing && !g_ents[r].is_dir && !strcmp(g_cwd, cur_dir()) && cur_file()[0] &&
                             !strcmp(g_ents[r].name, cur_file());
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
            if (cur_file()[0] && g_playing) fit(title, sizeof title, cur_file(), COLS);
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
            {                                                       // shuffle and repeat
                static const char *rl[3] = { "REPEAT: OFF", "REPEAT: FOLDER", "REPEAT: SONG" };
                int hw = W / 2 - 16;
                draw_rect(&s, 8, MODE_Y, hw, MODE_H, pressed_btn == 5 ? 0x5A80B8 : (g_shuffle ? 0x2E7D4F : 0x2A3850));
                draw_text(&s, 8 + (hw - 16 * 12) / 2, MODE_Y + (MODE_H - 16) / 2, 2, g_shuffle ? "SHUFFLE: ON" : "SHUFFLE: OFF", 0xFFFFFF);
                draw_rect(&s, W / 2 + 8, MODE_Y, hw, MODE_H, pressed_btn == 6 ? 0x5A80B8 : (g_repeat ? 0x2E7D4F : 0x2A3850));
                int rn = (int)strlen(rl[g_repeat]);
                draw_text(&s, W / 2 + 8 + (hw - 16 * rn) / 2, MODE_Y + (MODE_H - 16) / 2, 2, rl[g_repeat], 0xFFFFFF);
            }
#ifndef __APPLE__
            {                                                       // the device volume: drag, or use the volume keys
                char vl[24]; snprintf(vl, sizeof vl, "VOLUME %d%%", master);
                draw_text(&s, VOL_X0, VOL_Y + 2, 2, vl, 0x99AABB);
                draw_rect(&s, VOL_X0, VOL_Y + 36, VOL_X1 - VOL_X0, 36, 0x2A3140);
                draw_rect(&s, VOL_X0, VOL_Y + 36, (VOL_X1 - VOL_X0) * master / 100, 36, 0x77CC88);
            }
#else
            draw_text(&s, VOL_X0, VOL_Y + 20, 2, "Volume: use the volume buttons", 0x99AABB);
#endif

            plat_flip();
            dirty--;
        } else {
            usleep(12000);
        }
    }
    LOG("exit: normal");
#ifdef __APPLE__
    stop_audio();
#endif
    plat_close();                                       // Android: the music goes on in the background process
    return 0;
}
