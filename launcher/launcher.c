// launcher.c - touch app picker for the framebuffer tools (iPhone and Android).
// Tap a tile to run a tool. When the tool ends, you are back here. The TIC-80 tile opens a list of carts.
//
// The launcher keeps the display (and, on the iPhone, SpringBoard's silent mode) for its whole run. A tool that
// the launcher starts has FB_PARENT set, so it leaves the flag and the picture to the launcher when it ends.
// usage: fblauncher [seconds]     (default 86400)
//
// Kiosk mode (FB_KIOSK set): the Exit tile is not shown, so there is no way out to the phone's home screen.
// Use launcher/kiosk.sh from the Mac to turn kiosk mode off.
#include "../common/plat.h"
#include "../common/draw.h"
#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef __APPLE__
#define BASE "/var/root"
#define DEVICE_NAME "iPhone 4"
#else
#define BASE "/data/local/tmp"
#define DEVICE_NAME "Android"
#endif

enum { K_RUN, K_CARTS, K_QUIT, K_SHUTDOWN };
typedef struct { const char *label, *sub, *glyph; uint32_t color; const char *cmd; int kind; } Tool;

static const Tool TOOLS[] = {
#ifdef __APPLE__
    { "Paint",     "draw with fingers",   "~",   0x3A6EA5, BASE "/paint 3600 " BASE "/data/paint", K_RUN },
    { "Shell",     "terminal + keyboard", ">_",  0x3AA56E, BASE "/fbshell 3600 2",        K_RUN },
    { "TIC-80",    "pick a game cart",    "80",  0xA53A4E, BASE "/tic80",                 K_CARTS },
    { "Music",     "play your music",     "|||", 0xA58A3A, BASE "/musicplayer " BASE "/music 86400", K_RUN },
    { "Notes",     "notes + to-do lists", "[x]", 0x3A9AA5, BASE "/notes 86400 " BASE "/data/notes", K_RUN },
    { "Timer",     "watch, timer, alarm", "o",   0xA55A3A, BASE "/timer 86400",           K_RUN },
    { "Bible",     "American Standard",   "+",   0x7A5A3A, BASE "/bible 86400 " BASE "/data/bible/asv.txt", K_RUN },
    { "Reader",    "txt + epub books",    "Bk",  0x5A7A3A, BASE "/reader 86400 " BASE "/data/books", K_RUN },
    { "Pixel",     "pixel art editor",    "::",  0xA53A8A, BASE "/pixel 86400 " BASE "/data/pixel", K_RUN },
    { "Chat",      "Longhouse family chat", "Lh", 0x3A6EA5, BASE "/longhouse 86400 " BASE "/data/longhouse", K_RUN },
    { "Calibrate", "fix touch axes",      "+",   0x6E3AA5, BASE "/calib",                 K_RUN },
#else
    { "Paint",     "draw with fingers",   "~",   0x3A6EA5, BASE "/paint 3600 " BASE "/data/paint", K_RUN },
    { "Shell",     "terminal + keyboard", ">_",  0x3AA56E, BASE "/fbshell 3600 2",        K_RUN },
    { "Music",     "play your music",     "|||", 0xA58A3A, BASE "/musicplayer " BASE "/music 86400", K_RUN },
    { "Notes",     "notes + to-do lists", "[x]", 0x3A9AA5, BASE "/notes 86400 " BASE "/data/notes", K_RUN },
    { "Timer",     "watch, timer, alarm", "o",   0xA55A3A, BASE "/timer 86400",           K_RUN },
    { "Bible",     "American Standard",   "+",   0x7A5A3A, BASE "/bible 86400 " BASE "/data/bible/asv.txt", K_RUN },
    { "Reader",    "txt + epub books",    "Bk",  0x5A7A3A, BASE "/reader 86400 " BASE "/data/books", K_RUN },
    { "TIC-80",    "pick a game cart",    "80",  0xA53A4E, BASE "/tic80",                 K_CARTS },
    { "Pixel",     "pixel art editor",    "::",  0xA53A8A, BASE "/pixel 86400 " BASE "/data/pixel", K_RUN },
    { "Chat",      "Longhouse family chat", "Lh", 0x3A6EA5, BASE "/longhouse 86400 " BASE "/data/longhouse", K_RUN },
    { "Console",   "text demo",           "Aa",  0xA58A3A, BASE "/condemo 20 2",          K_RUN },
    { "Shut down", "turn the device off", "o|",  0x6A2A2A, "reboot -p",                   K_SHUTDOWN },
#endif
#ifdef __APPLE__
    { "Shut down", "turn the phone off",  "o|",  0x6A2A2A, "/sbin/halt",                  K_SHUTDOWN },
#endif
    { "Restart",   "back to the phone",   "x",   0x883333, NULL,                          K_QUIT },     // Shut down and Restart sit in the power menu, not in the grid
};
#define N_ALL ((int)(sizeof TOOLS / sizeof TOOLS[0]))

// Copy src into dst, cut to `cols` characters with a "~" at the end when it is too long.
static void fit(char *dst, size_t n, const char *src, int cols) {
    snprintf(dst, n, "%s", src);
    if (cols > 0 && (int)strlen(dst) > cols) { dst[cols - 1] = '~'; dst[cols] = 0; }
}

static int inside(int px, int py, int x, int y, int w, int h) { return px >= x && px < x + w && py >= y && py < y + h; }

static double now(void) { struct timeval tv; gettimeofday(&tv, NULL); return tv.tv_sec + tv.tv_usec / 1e6; }

// Run a command line as a child, with FB_PARENT set, and wait for it. The flag stays fresh meanwhile.
static void run_tool(const char *cmdline) {
    char buf[512];
    snprintf(buf, sizeof buf, "%s", cmdline);
    char *argv[16]; int n = 0;
    for (char *p = strtok(buf, " "); p && n < 15; p = strtok(NULL, " ")) argv[n++] = p;
    argv[n] = NULL;
    if (n == 0 || access(argv[0], X_OK) != 0) return;
    pid_t pid = fork();
    if (pid < 0) return;
    if (pid == 0) { setenv("FB_PARENT", "1", 1); execv(argv[0], argv); _exit(127); }
    int st;
    for (;;) {
        pid_t r = waitpid(pid, &st, WNOHANG);
        if (r == pid || (r < 0 && errno != EINTR)) break;
        plat_keepalive();
        usleep(100000);
    }
}

// After a tool ends, its last taps are still queued. Read and discard touches for a moment, then wait for fingers up.
static void settle(void) {
    double t0 = now();
    PlatTouch t[10];
    while (now() - t0 < 0.5) { plat_touch_poll(t, 10); usleep(10000); }
    while (plat_touch_poll(t, 10) > 0 && now() - t0 < 3.0) usleep(10000);
}

typedef struct { char name[200]; } Cart;
static Cart *g_carts; static int g_ncarts;

static int has_ext(const char *n, const char *e) {
    size_t a = strlen(n), b = strlen(e);
    return a > b && strcasecmp(n + a - b, e) == 0;
}
static int cmp_cart(const void *a, const void *b) { return strcasecmp(((const Cart *)a)->name, ((const Cart *)b)->name); }
static void scan_carts(void) {
    free(g_carts); g_carts = NULL; g_ncarts = 0;
    int cap = 32; g_carts = malloc(cap * sizeof(Cart));
    DIR *d = opendir(BASE "/carts");
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.' || !(has_ext(e->d_name, ".lua") || has_ext(e->d_name, ".tic"))) continue;
        if (g_ncarts == cap) { cap *= 2; g_carts = realloc(g_carts, cap * sizeof(Cart)); }
        snprintf(g_carts[g_ncarts++].name, sizeof g_carts[0].name, "%s", e->d_name);
    }
    closedir(d);
    qsort(g_carts, g_ncarts, sizeof(Cart), cmp_cart);
}

#ifndef __APPLE__
#define FONT_DIR BASE "/data/fonts"
// The reading fonts on the phone: *.ttf and *.otf in FONT_DIR. The one in use is named in FONT_DIR/choice.
static int scan_fonts(char fonts[][64], int max, int *cur) {
    int n = 0; *cur = 0;
    DIR *d = opendir(FONT_DIR); if (!d) return 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < max) {
        size_t l = strlen(e->d_name);
        if (e->d_name[0] == '.' || l < 5 || l > 60 || (strcasecmp(e->d_name + l - 4, ".ttf") && strcasecmp(e->d_name + l - 4, ".otf"))) continue;
        snprintf(fonts[n++], 64, "%s", e->d_name);
    }
    closedir(d);
    for (int i = 0; i < n; i++) for (int j = i + 1; j < n; j++) if (strcasecmp(fonts[i], fonts[j]) > 0) { char t[64]; memcpy(t, fonts[i], 64); memcpy(fonts[i], fonts[j], 64); memcpy(fonts[j], t, 64); }
    char name[80] = ""; FILE *f = fopen(FONT_DIR "/choice", "r");
    if (f) { if (fgets(name, sizeof name, f)) name[strcspn(name, "\r\n")] = 0; fclose(f); }
    for (int i = 0; i < n; i++) if (!strcmp(fonts[i], name)) *cur = i + 1;
    return n;
}
static void save_font_choice(const char *name) { FILE *f = fopen(FONT_DIR "/choice", "w"); if (f) { fputs(name, f); fputc('\n', f); fclose(f); chmod(FONT_DIR "/choice", 0666); } }
#endif

int main(int argc, char **argv) {
    double seconds = argc > 1 ? atof(argv[1]) : 86400;
    const int kiosk = getenv("FB_KIOSK") != NULL;
    PlatInfo pi;
    if (plat_init(&pi) || plat_touch_init(&pi)) return 1;
    if (kiosk) { FILE *kf = fopen("/tmp/fbmode", "w"); if (kf) { fputc('K', kf); fclose(kf); } }   // a non-empty flag = kiosk: the tweak also blocks the buttons
    const int W = pi.w, H = pi.h;

    // only show tools whose program is installed (the cart picker always shows, the Exit tile only outside kiosk mode)
    int shown[16], nshown = 0;
    for (int i = 0; i < N_ALL && nshown < 16; i++) {
        if (TOOLS[i].kind == K_QUIT || TOOLS[i].kind == K_SHUTDOWN) continue;      // these two are in the power menu
        if (kiosk && strstr(TOOLS[i].cmd, "/calib")) continue;               // a bad calibration would lock the user out
        char buf[512]; snprintf(buf, sizeof buf, "%s", TOOLS[i].cmd);
        char *sp = strchr(buf, ' '); if (sp) *sp = 0;
        if (access(buf, X_OK) == 0) shown[nshown++] = i;
    }

    const int GAP = 20, HDR = 130;
    const int COLS = nshown <= 8 ? 2 : nshown <= 15 ? 3 : 4;                    // more apps: more columns, so the tiles shrink
    const int TG = COLS > 2 ? 12 : GAP;                                         // the gap between tiles
    const int tile_w = (W - (COLS + 1) * TG) / COLS;
    const int rows = (nshown + COLS - 1) / COLS;
    int tile_h = (H - HDR - TG * (rows + 1)) / (rows ? rows : 1);
    if (tile_h > 210) tile_h = 210;

    enum { S_MAIN, S_CARTS, S_SHUT } state = S_MAIN;
    int scroll = 0, dirty = 3, pressed = -1, menu = 0;
#ifndef __APPLE__
    // Settings drop-down, opened by tapping the name in the title bar. Rows: Wi-Fi and Bluetooth switches, volume, brightness,
    // portrait or landscape, and (outside boot mode) a button for Android's own Settings.
    int settings = 0, wifi = -1, bt = -1, vol = 100, bright = 100, drag = 0, nfont = 0, curfont = 0, fsize = 90;
    char fonts[12][64];                                                          // reading font files; index 0 of the cycle is the chunky font     // drag: 0 none, 1 volume slider, 2 brightness slider
    const int SP_W = W - 2 * GAP < 540 ? W - 2 * GAP : 540, SP_X = GAP, SP_Y = 92, SP_ROW = H < 700 ? 62 : 84, sp_rows = kiosk ? 6 : 7;
    const int SL_X0 = SP_X + 190, SL_X1 = SP_X + SP_W - 112;                      // the slider tracks
#endif
    double bat_until = 0;                                                       // while set, a box shows the exact battery percentage
    const int BAT_X = W - GAP - 126 - 70, PW_X = BAT_X - 76, PW_Y = 14, PW = 64;                    // the power button in the title bar, next to the time
    const int MENU_W = W - 2 * GAP < 380 ? W - 2 * GAP : 380, MENU_X = W - GAP - MENU_W, MENU_Y = 92, MENU_ROW = 90;
    const int menu_rows = kiosk ? 1 : 2;                                         // Restart ends the picker, so it is not offered at boot
    double t0 = now();

    int prev = 0, down_x = 0, down_y = 0, last_y = 0, moved = 0;
    double down_t = 0;
    const int ROW = 64, LIST_Y0 = 90;

    while (now() - t0 < seconds) {
        PlatTouch tc[10];
        int n = plat_touch_poll(tc, 10);
        int cur = n > 0;
        int tap = 0, tx = 0, ty = 0;
        if (cur && !prev) { down_x = tc[0].x; down_y = tc[0].y; last_y = down_y; down_t = now(); moved = 0; pressed = -2; dirty = 3; }
#ifndef __APPLE__
        if (cur && !prev && state == S_MAIN && settings && !menu) {                  // a touch that starts on a slider drags it
            if (inside(down_x, down_y, SL_X0 - 16, SP_Y + 4 + SP_ROW, SL_X1 - SL_X0 + 32, SP_ROW)) drag = 1;
            else if (inside(down_x, down_y, SL_X0 - 16, SP_Y + 4 + 2 * SP_ROW, SL_X1 - SL_X0 + 32, SP_ROW)) drag = 2;
            else if (curfont > 0 && inside(down_x, down_y, SL_X0 - 16, SP_Y + 4 + 5 * SP_ROW, SL_X1 - SL_X0 + 32, SP_ROW)) drag = 3;
        }
        if (cur && drag) {
            int v = (tc[0].x - SL_X0) * 100 / (SL_X1 - SL_X0);
            if (drag == 1) { vol = v < 0 ? 0 : v > 100 ? 100 : v; }
            else if (drag == 3) { fsize = 60 + (v < 0 ? 0 : v > 100 ? 100 : v) * 70 / 100; ui_font_set_pct(fsize); }          // menu text size, live
            else { bright = v < 5 ? 5 : v > 100 ? 100 : v; plat_brightness_set(bright); }                    // brightness changes while you drag
            dirty = 3;
        }
        if (!cur && prev && drag) { if (drag == 1) plat_volume_set(vol); else if (drag == 3) ui_font_save_pct(); drag = 0; dirty = 3; }
#endif
        if (cur && prev) {
            if (abs(tc[0].y - down_y) > 18 || abs(tc[0].x - down_x) > 18) moved = 1;
            if (state == S_CARTS && moved) { scroll -= tc[0].y - last_y; dirty = 3; }
            last_y = tc[0].y;
        }
        if (!cur && prev) {                                              // touch ended: a short, still touch is a tap
            if (!moved && now() - down_t < 0.8) { tap = 1; tx = down_x; ty = down_y; }
            pressed = -1; dirty = 3;
        }
        prev = cur;

        int quit = 0;
        if (tap && state == S_MAIN && menu) {
            if (tx >= MENU_X && tx < MENU_X + MENU_W && ty >= MENU_Y && ty < MENU_Y + menu_rows * MENU_ROW) {
                int row = (ty - MENU_Y) / MENU_ROW;
                if (!kiosk && row == 0) quit = 1;                               // Restart
                else state = S_SHUT;                                            // Shut down asks again on its own screen
            }
            menu = 0; dirty = 3;
#ifndef __APPLE__
        } else if (tap && state == S_MAIN && settings) {
            if (inside(tx, ty, SP_X, SP_Y, SP_W, sp_rows * SP_ROW + 8)) {
                int row = (ty - SP_Y - 4) / SP_ROW;
                if (row == 0 && tx < SP_X + SP_W / 2 && tx >= SP_X + SP_W / 2 - 110 && wifi >= 0) { plat_wifi_set(!wifi); usleep(600000); wifi = plat_wifi_state(); }
                else if (row == 0 && tx >= SP_X + SP_W - 122 && bt >= 0) { plat_bt_set(!bt); usleep(300000); bt = plat_bt_state(); }
                else if (row == 3) {                                              // portrait or landscape: the picker starts again to use it
                    int want_land = tx >= SP_X + SP_W / 2, now_land = W > H;
                    if (want_land != now_land) {
                        plat_orientation_set(want_land);
                        char self[256]; ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
                        if (n > 0) { self[n] = 0; plat_close(); execv(self, argv); }
                    }
                } else if (row == 4 && nfont > 0) {                              // reading font: chunky, then each font file in turn
                    curfont = (curfont + 1) % (nfont + 1);
                    save_font_choice(curfont ? fonts[curfont - 1] : ""); ui_font_reload();
                } else if (row == 6 && !kiosk) {                                  // leave the picker and open the Android screen
                    FILE *af = fopen("/data/local/tmp/after_start", "w");
                    if (af) { fputs("android.settings.SETTINGS", af); fclose(af); quit = 1; }
                }
            } else settings = 0;
            dirty = 3;
        } else if (tap && state == S_MAIN && tx < GAP + 320 && ty < MENU_Y) {
            settings = 1; wifi = plat_wifi_state(); bt = plat_bt_state(); nfont = scan_fonts(fonts, 12, &curfont); fsize = g_uif_pct; vol = plat_volume_get(); { int b = plat_brightness_get(); bright = b < 0 ? 100 : b; } dirty = 3;
#endif
        } else if (tap && state == S_MAIN && tx >= BAT_X - 8 && tx < BAT_X + 66 && ty < MENU_Y) {
            bat_until = now() + 4; dirty = 3;                                    // tap the battery: show the percentage for a few seconds
        } else if (tap && state == S_MAIN && tx >= PW_X - 8 && tx < PW_X + PW + 8 && ty < MENU_Y) {
            menu = 1; dirty = 3;
#ifndef __APPLE__
            settings = 0;
#endif
        } else if (tap && state == S_MAIN) {
            for (int k = 0; k < nshown; k++) {
                int x = TG + (k % COLS) * (tile_w + TG), y = HDR + TG + (k / COLS) * (tile_h + TG);
                if (tx < x || tx >= x + tile_w || ty < y || ty >= y + tile_h) continue;
                const Tool *t = &TOOLS[shown[k]];
                if (t->kind == K_QUIT) quit = 1;
                else if (t->kind == K_SHUTDOWN) state = S_SHUT;
                else if (t->kind == K_CARTS) { scan_carts(); scroll = 0; state = S_CARTS; }
                else { run_tool(t->cmd); settle(); prev = 0; }
                dirty = 3; break;
            }
        } else if (tap && state == S_SHUT) {
            int bx = GAP, bw = W - 2 * GAP;
            if (tx >= bx && tx < bx + bw && ty >= 380 && ty < 560) {                 // YES
                dirty = 0; state = S_MAIN;
                int stride; uint32_t *px = plat_begin(&stride);
                Surf s = { px, stride, W, H, pi.rs, pi.gs, pi.bs, pi.alpha, pi.theme };
                draw_rect(&s, 0, 0, W, H, 0x000000);
                draw_text(&s, GAP + 6, H / 2 - 16, 3, "Shutting down...", 0xFFFFFF);
                plat_flip();
                sync();
                for (int q = 0; q < N_ALL; q++) if (TOOLS[q].kind == K_SHUTDOWN) { system(TOOLS[q].cmd); break; }       // /sbin/halt on iPhone, "reboot -p" on Android
                sleep(60);
            } else if (tx >= bx && tx < bx + bw && ty >= 600 && ty < 780) state = S_MAIN;     // CANCEL
            dirty = 3;
        } else if (tap && state == S_CARTS) {
            if (ty < LIST_Y0 && tx < 200) { state = S_MAIN; }                       // BACK button in the header
            else if (ty >= LIST_Y0) {
                int row = (ty - LIST_Y0 + scroll) / ROW;
                if (row >= 0 && row < g_ncarts) {
                    char cmd[512];
                    snprintf(cmd, sizeof cmd, "%s/tic80 %s/carts/%s 3600", BASE, BASE, g_carts[row].name);
                    run_tool(cmd); settle(); prev = 0;
                }
            }
            dirty = 3;
        }
        if (quit) break;

        if (state == S_CARTS) {
            int max_scroll = g_ncarts * ROW - (H - LIST_Y0); if (max_scroll < 0) max_scroll = 0;
            if (scroll < 0) scroll = 0; if (scroll > max_scroll) scroll = max_scroll;
        }

        if (bat_until && now() > bat_until) { bat_until = 0; dirty = 3; }
        { static int last_min = -1; time_t tt = time(NULL); struct tm lt; localtime_r(&tt, &lt); if (lt.tm_min != last_min) { last_min = lt.tm_min; dirty = 3; } }
        if (dirty > 0) {
            int stride; uint32_t *px = plat_begin(&stride);
            Surf s = { px, stride, W, H, pi.rs, pi.gs, pi.bs, pi.alpha, pi.theme };
            draw_rect(&s, 0, 0, W, H, 0x101820);
            if (state == S_MAIN) {
                // title bar (ink) with the name and the time, like a palmtop; with the theme off it is the old dark header
                draw_rect(&s, 0, 0, W, 92, 0x26303E);
                draw_text(&s, GAP + 6, 26, 5, "momPKT", 0xFFFFFF);
#ifndef __APPLE__
                draw_text(&s, GAP + 6 + 6 * 40 + 12, 30, 3, "v", 0xFFFFFF);                  // the settings drop-down hangs from the name
#endif
                { time_t tt = time(NULL); struct tm lt; localtime_r(&tt, &lt); char clock[8]; strftime(clock, sizeof clock, "%H:%M", &lt);
                  draw_text(&s, W - GAP - 5 * 8 * 3 - 6, 30, 3, clock, 0xFFFFFF); }
                { int pct, chg;                                                  // the battery, left of the time; nothing if the phone shows none
                  if (plat_battery(&pct, &chg) == 0) {
                      uint32_t fg = s.theme ? 0x000000 : 0xFFFFFF;
                      int bx = BAT_X, by = 30, bw = 52, bh = 28;
                      draw_rect(&s, bx, by, bw, bh, fg); draw_rect(&s, bx + bw, by + 8, 5, bh - 16, fg);           // the body and the nub
                      draw_rect(&s, bx + 3, by + 3, bw - 6, bh - 6, s.theme ? 0xFFFFFF : 0x26303E);
                      int fw = (bw - 10) * pct / 100; if (pct > 0 && fw < 2) fw = 2;
                      draw_rect(&s, bx + 5, by + 5, fw, bh - 10, pct <= 15 && !s.theme ? 0xE05050 : fg);
                      if (chg) {                                                   // a plus, centered in the body, in the colour that shows against what is under it
                          uint32_t pc = fw > bw / 2 - 5 ? (s.theme ? 0xFFFFFF : 0x26303E) : fg;
                          draw_rect(&s, bx + bw / 2 - 8, by + bh / 2 - 2, 16, 4, pc); draw_rect(&s, bx + bw / 2 - 2, by + bh / 2 - 8, 4, 16, pc);
                      }
                  } }
                { static const char *POWER[12] = { ".....##.....", ".....##.....", "..#..##..#..", ".##..##..##.", "##...##...##", "##...##...##",
                                                   "##........##", "##........##", ".##......##.", "..##....##..", "...######...", "............" };
                  uint32_t ink = s.theme ? 0x000000 : 0xFFFFFF;                  // in the palm look the bar is dark ink, so the icon is paper
                  if (menu) { draw_rect(&s, PW_X, PW_Y, PW, PW, ink); ink = s.theme ? 0xFFFFFF : 0x26303E; }
                  for (int r = 0; r < 12; r++) for (int c = 0; c < 12; c++) if (POWER[r][c] == '#') draw_rect(&s, PW_X + 8 + c * 4, PW_Y + 8 + r * 4, 4, 4, ink); }
#ifndef __APPLE__
                int overlay_open = menu || settings;                       // a touch on a drop-down must not light the tile under it
#else
                int overlay_open = menu;
#endif
                for (int k = 0; k < nshown; k++) {
                    const Tool *t = &TOOLS[shown[k]];
                    int x = TG + (k % COLS) * (tile_w + TG), y = HDR + TG + (k / COLS) * (tile_h + TG);
                    int down = !overlay_open && cur && pressed == -2 && down_x >= x && down_x < x + tile_w && down_y >= y && down_y < y + tile_h;
                    int gl = (int)strlen(t->glyph), ll = (int)strlen(t->label), sl = (int)strlen(t->sub);
                    // the glyph, name and small line shrink to fit the tile, so a full screen of apps still reads
                    int gs = 6; while (gs > 1 && (gl * 8 * gs > tile_w - 16 || 16 * gs > tile_h * 45 / 100)) gs--;
                    int ls = 3; while (ls > 1 && (ll * 8 * ls > tile_w - 12 || 16 * ls > tile_h * 28 / 100)) ls--;
                    int show_sub = tile_h >= 150, lcols = (tile_w - 12) / (8 * ls), scols = (tile_w - 12) / 8;
                    char lab[40], sub[64]; fit(lab, sizeof lab, t->label, lcols); fit(sub, sizeof sub, t->sub, scols);
                    int total = 16 * gs + 10 + 16 * ls + (show_sub ? 14 : 0), ty0 = y + (tile_h - total) / 2;
                    uint32_t edge = s.theme ? 0xFFFFFF : t->color;
                    draw_rect(&s, x, y, tile_w, tile_h, edge);                  // themed: an ink outline, paper inside (inverted while pressed)
                    if (s.theme) draw_rect(&s, x + 4, y + 4, tile_w - 8, tile_h - 8, down ? 0xFFFFFF : 0x000000);
                    else { draw_rect(&s, x, y + tile_h - 6, tile_w, 6, 0x000000); if (down) { draw_rect(&s, x, y, tile_w, 8, 0xFFFFFF); draw_rect(&s, x, y + tile_h - 8, tile_w, 8, 0xFFFFFF); draw_rect(&s, x, y, 8, tile_h, 0xFFFFFF); draw_rect(&s, x + tile_w - 8, y, 8, tile_h, 0xFFFFFF); } }
                    draw_text(&s, x + (tile_w - gl * 8 * gs) / 2, ty0, gs, t->glyph, 0xFFFFFF);
                    draw_text(&s, x + (tile_w - (int)strlen(lab) * 8 * ls) / 2, ty0 + 16 * gs + 10, ls, lab, 0xFFFFFF);
                    if (show_sub) draw_text(&s, x + (tile_w - (int)strlen(sub) * 8) / 2, ty0 + 16 * gs + 10 + 16 * ls + 6, 1, sub, s.theme ? 0x8899AA : 0xDDDDDD);
                    (void)sl;
                }
#ifndef __APPLE__
                if (settings) {
                    uint32_t edge = s.theme ? 0xFFFFFF : 0x9AAABB, inner = s.theme ? 0x000000 : 0x202C3C, dim = s.theme ? 0x8899AA : 0xDDDDDD;
                    draw_rect(&s, SP_X, SP_Y, SP_W, sp_rows * SP_ROW + 8, edge);
                    draw_rect(&s, SP_X + 4, SP_Y + 4, SP_W - 8, sp_rows * SP_ROW, inner);
                    for (int r = 1; r < sp_rows; r++) draw_rect(&s, SP_X + 4, SP_Y + 4 + r * SP_ROW - 2, SP_W - 8, 4, edge);
                    int ty0 = SP_Y + 4;
                    // row 0: Wi-Fi and Bluetooth switches, side by side
                    for (int k = 0; k < 2; k++) {
                        int st = k ? bt : wifi, hx = SP_X + 4 + k * (SP_W / 2 - 2);
                        draw_text(&s, hx + 16, ty0 + (SP_ROW - 32) / 2, 2, k ? "BT" : "Wi-Fi", 0xFFFFFF);
                        if (st >= 0) {
                            int bw = 98, bx = hx + SP_W / 2 - 4 - bw - 16, by = ty0 + 14, bh = SP_ROW - 28;
                            draw_rect(&s, bx, by, bw, bh, edge); draw_rect(&s, bx + 4, by + 4, bw - 8, bh - 8, st ? edge : inner);
                            draw_text(&s, bx + (bw - (st ? 2 : 3) * 16) / 2, by + (bh - 32) / 2, 2, st ? "ON" : "OFF", st ? inner : 0xFFFFFF);
                        } else draw_text(&s, hx + 16 + (k ? 3 : 6) * 16 + 8, ty0 + (SP_ROW - 16) / 2, 1, "n/a", dim);
                    }
                    // rows 1 and 2: volume and brightness sliders
                    for (int k = 0; k < 2; k++) {
                        int y1 = ty0 + (k + 1) * SP_ROW, val = k ? bright : vol; char vb[16]; snprintf(vb, sizeof vb, "%d%%", val);
                        draw_text(&s, SP_X + 20, y1 + (SP_ROW - 32) / 2, 2, k ? "Light" : "Sound", 0xFFFFFF);
                        draw_rect(&s, SL_X0, y1 + SP_ROW / 2 - 4, SL_X1 - SL_X0, 8, edge);
                        draw_rect(&s, SL_X0 + (SL_X1 - SL_X0) * val / 100 - 12, y1 + SP_ROW / 2 - 20, 24, 40, edge);      // the knob
                        draw_text(&s, SL_X1 + 20, y1 + (SP_ROW - 32) / 2, 2, vb, 0xFFFFFF);
                    }
                    // row 2: portrait or landscape, the current one is filled
                    int y2 = ty0 + 3 * SP_ROW, land = W > H, hw = SP_W / 2 - 16;
                    for (int k = 0; k < 2; k++) {
                        int bx = SP_X + 12 + k * (SP_W / 2), by = y2 + 12, bh = SP_ROW - 24, sel = (k == 1) == land;
                        draw_rect(&s, bx, by, hw, bh, edge); draw_rect(&s, bx + 4, by + 4, hw - 8, bh - 8, sel ? edge : inner);
                        const char *nm = k ? "LANDSCAPE" : "PORTRAIT";
                        draw_text(&s, bx + (hw - (int)strlen(nm) * 16) / 2, by + (bh - 32) / 2, 2, nm, sel ? inner : 0xFFFFFF);
                    }
                    // row 4: the reading font for Reader and Bible (the menus always keep the chunky font)
                    {
                        int y4 = ty0 + 4 * SP_ROW, bx = SP_X + 150, by = y4 + 10, bw = SP_W - 150 - 12, bh = SP_ROW - 20;
                        draw_text(&s, SP_X + 20, y4 + (SP_ROW - 32) / 2, 2, "Font", 0xFFFFFF);
                        if (nfont > 0) {
                            char nm[40]; snprintf(nm, sizeof nm, "%s", curfont ? fonts[curfont - 1] : "Chunky");
                            char *dot = strrchr(nm, '.'); if (dot) *dot = 0;
                            int maxc = (bw - 16) / 16; if ((int)strlen(nm) > maxc) { nm[maxc > 1 ? maxc - 1 : 0] = '~'; nm[maxc > 1 ? maxc : 1] = 0; }
                            draw_rect(&s, bx, by, bw, bh, edge); draw_rect(&s, bx + 4, by + 4, bw - 8, bh - 8, inner);
                            draw_text(&s, bx + (bw - (int)strlen(nm) * 16) / 2, by + (bh - 32) / 2, 2, nm, 0xFFFFFF);
                        } else draw_text(&s, bx, y4 + (SP_ROW - 16) / 2, 1, "no fonts installed", dim);
                    }
                    // row 5: the size of the menu font (only when a font is chosen)
                    {
                        int y5 = ty0 + 5 * SP_ROW; char vb[16]; snprintf(vb, sizeof vb, "%d%%", fsize);
                        draw_text(&s, SP_X + 20, y5 + (SP_ROW - 32) / 2, 2, "Size", curfont > 0 ? 0xFFFFFF : 0x778899);
                        if (curfont > 0) {
                            draw_rect(&s, SL_X0, y5 + SP_ROW / 2 - 4, SL_X1 - SL_X0, 8, edge);
                            draw_rect(&s, SL_X0 + (SL_X1 - SL_X0) * (fsize - 60) / 70 - 12, y5 + SP_ROW / 2 - 20, 24, 40, edge);
                            draw_text(&s, SL_X1 + 20, y5 + (SP_ROW - 32) / 2, 2, vb, 0xFFFFFF);
                        } else draw_text(&s, SL_X0, y5 + (SP_ROW - 16) / 2, 1, "pick a font first", dim);
                    }
                    // row 6: Android's own Settings app, for networks and Bluetooth (not in boot mode, where there is no way out)
                    if (!kiosk) {
                        int y3 = ty0 + 6 * SP_ROW, bx = SP_X + 12, by = y3 + 12, bw = SP_W - 24, bh = SP_ROW - 24;
                        draw_rect(&s, bx, by, bw, bh, edge); draw_rect(&s, bx + 4, by + 4, bw - 8, bh - 8, inner);
                        const char *nm = "ANDROID SETTINGS";
                        draw_text(&s, bx + (bw - (int)strlen(nm) * 16) / 2, by + (bh - 32) / 2, 2, nm, 0xFFFFFF);
                    }
                }
#endif
                if (bat_until) {
                    int pct, chg;
                    if (plat_battery(&pct, &chg) == 0) {
                        char b1[16], b2[24]; snprintf(b1, sizeof b1, "%d%%", pct); snprintf(b2, sizeof b2, "%s", chg ? "charging" : "on battery");
                        int bw2 = 220, bx2 = W - GAP - bw2, by2 = 96;
                        draw_rect(&s, bx2, by2, bw2, 96, s.theme ? 0xFFFFFF : 0x9AAABB);
                        draw_rect(&s, bx2 + 4, by2 + 4, bw2 - 8, 88, s.theme ? 0x000000 : 0x202C3C);
                        draw_text(&s, bx2 + (bw2 - (int)strlen(b1) * 24) / 2, by2 + 14, 3, b1, 0xFFFFFF);
                        draw_text(&s, bx2 + (bw2 - (int)strlen(b2) * 8) / 2, by2 + 66, 1, b2, s.theme ? 0x8899AA : 0xDDDDDD);
                    }
                }
                if (menu) {                                                      // the drop-down: which one?
                    draw_rect(&s, MENU_X, MENU_Y, MENU_W, menu_rows * MENU_ROW + 8, s.theme ? 0xFFFFFF : 0x9AAABB);
                    draw_rect(&s, MENU_X + 4, MENU_Y + 4, MENU_W - 8, menu_rows * MENU_ROW, s.theme ? 0x000000 : 0x202C3C);
                    for (int r = 0; r < menu_rows; r++) {
                        const char *name = (!kiosk && r == 0) ? "Restart" : "Shut down";
                        if (r) draw_rect(&s, MENU_X + 4, MENU_Y + 4 + r * MENU_ROW - 2, MENU_W - 8, 4, s.theme ? 0xFFFFFF : 0x9AAABB);
                        draw_text(&s, MENU_X + 24, MENU_Y + 4 + r * MENU_ROW + (MENU_ROW - 48) / 2, 3, name, 0xFFFFFF);
                    }
                }
            } else if (state == S_SHUT) {
                draw_text(&s, GAP + 6, 200, 4, "SHUT DOWN?", 0xFFFFFF);
                draw_text(&s, GAP + 6, 280, 2, "The phone turns off. Push the power", 0x99AABB);
                draw_text(&s, GAP + 6, 312, 2, "button to start it again.", 0x99AABB);
                draw_rect(&s, GAP, 380, W - 2 * GAP, 180, 0x9A2A2A);
                draw_text(&s, GAP + 30, 440, 5, "YES, OFF", 0xFFFFFF);
                draw_rect(&s, GAP, 600, W - 2 * GAP, 180, 0x2A3850);
                draw_text(&s, GAP + 30, 660, 5, "CANCEL", 0xFFFFFF);
            } else {
                if (g_ncarts == 0) draw_text(&s, 20, LIST_Y0 + 20, 2, "No carts in " BASE "/carts", 0x888888);
                for (int r = 0; r < g_ncarts; r++) {
                    int y = LIST_Y0 + r * ROW - scroll;
                    if (y + ROW <= LIST_Y0 || y >= H) continue;
                    char line[200]; snprintf(line, sizeof line, "%s", g_carts[r].name);
                    int maxc = (W - 40) / 16; if ((int)strlen(line) > maxc) { line[maxc - 1] = '~'; line[maxc] = 0; }
                    draw_text(&s, 20, y + 16, 2, line, 0xFFFFFF);
                    draw_rect(&s, 0, y + ROW - 2, W, 1, 0x1E2733);
                }
                draw_rect(&s, 0, 0, W, LIST_Y0, 0x1B2330);
                draw_rect(&s, 0, 0, 200, LIST_Y0, 0x2A3850);
                draw_text(&s, 28, 28, 3, "BACK", 0xFFFFFF);
                draw_text(&s, 230, 34, 2, "pick a cart", 0x99AABB);
            }
            plat_flip();
            dirty--;
        } else {
            usleep(12000);
        }
    }
    plat_close();
    return 0;
}
