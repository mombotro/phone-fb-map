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
    { "Paint",     "draw with fingers",   "~",   0x3A6EA5, BASE "/paint 3600",            K_RUN },
    { "Shell",     "terminal + keyboard", ">_",  0x3AA56E, BASE "/fbshell 3600 2",        K_RUN },
    { "TIC-80",    "pick a game cart",    "80",  0xA53A4E, BASE "/tic80",                 K_CARTS },
    { "Music",     "play your music",     "|||", 0xA58A3A, BASE "/musicplayer " BASE "/music 86400", K_RUN },
    { "Notes",     "notes + to-do lists", "[x]", 0x3A9AA5, BASE "/notes 86400 " BASE "/data/notes", K_RUN },
    { "Timer",     "watch, timer, alarm", "o",   0xA55A3A, BASE "/timer 86400",           K_RUN },
    { "Bible",     "American Standard",   "+",   0x7A5A3A, BASE "/bible 86400 " BASE "/data/bible/asv.txt", K_RUN },
    { "Chat",      "Longhouse family chat", "Lh", 0x3A6EA5, BASE "/longhouse 86400 " BASE "/data/longhouse", K_RUN },
    { "Calibrate", "fix touch axes",      "+",   0x6E3AA5, BASE "/calib",                 K_RUN },
#else
    { "Paint",     "draw with fingers",   "~",   0x3A6EA5, BASE "/paint 3600",            K_RUN },
    { "Shell",     "terminal + keyboard", ">_",  0x3AA56E, BASE "/fbshell 3600 2",        K_RUN },
    { "Music",     "play your music",     "|||", 0xA58A3A, BASE "/musicplayer " BASE "/music 86400", K_RUN },
    { "Notes",     "notes + to-do lists", "[x]", 0x3A9AA5, BASE "/notes 86400 " BASE "/data/notes", K_RUN },
    { "Timer",     "watch, timer, alarm", "o",   0xA55A3A, BASE "/timer 86400",           K_RUN },
    { "Bible",     "American Standard",   "+",   0x7A5A3A, BASE "/bible 86400 " BASE "/data/bible/asv.txt", K_RUN },
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
    const int PW_X = W - GAP - 126 - 76, PW_Y = 14, PW = 64;                    // the power button in the title bar, next to the time
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
        } else if (tap && state == S_MAIN && tx >= PW_X - 8 && tx < PW_X + PW + 8 && ty < MENU_Y) {
            menu = 1; dirty = 3;
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

        { static int last_min = -1; time_t tt = time(NULL); struct tm lt; localtime_r(&tt, &lt); if (lt.tm_min != last_min) { last_min = lt.tm_min; dirty = 3; } }
        if (dirty > 0) {
            int stride; uint32_t *px = plat_begin(&stride);
            Surf s = { px, stride, W, H, pi.rs, pi.gs, pi.bs, pi.alpha, pi.theme };
            draw_rect(&s, 0, 0, W, H, 0x101820);
            if (state == S_MAIN) {
                // title bar (ink) with the name and the time, like a palmtop; with the theme off it is the old dark header
                draw_rect(&s, 0, 0, W, 92, 0x26303E);
                draw_text(&s, GAP + 6, 26, 5, "momPKT", 0xFFFFFF);
                { time_t tt = time(NULL); struct tm lt; localtime_r(&tt, &lt); char clock[8]; strftime(clock, sizeof clock, "%H:%M", &lt);
                  draw_text(&s, W - GAP - 5 * 8 * 3 - 6, 30, 3, clock, 0xFFFFFF); }
                { static const char *POWER[12] = { ".....##.....", ".....##.....", "..#..##..#..", ".##..##..##.", "##...##...##", "##...##...##",
                                                   "##........##", "##........##", ".##......##.", "..##....##..", "...######...", "............" };
                  uint32_t ink = s.theme ? 0x000000 : 0xFFFFFF;                  // in the palm look the bar is dark ink, so the icon is paper
                  if (menu) { draw_rect(&s, PW_X, PW_Y, PW, PW, ink); ink = s.theme ? 0xFFFFFF : 0x26303E; }
                  for (int r = 0; r < 12; r++) for (int c = 0; c < 12; c++) if (POWER[r][c] == '#') draw_rect(&s, PW_X + 8 + c * 4, PW_Y + 8 + r * 4, 4, 4, ink); }
                for (int k = 0; k < nshown; k++) {
                    const Tool *t = &TOOLS[shown[k]];
                    int x = TG + (k % COLS) * (tile_w + TG), y = HDR + TG + (k / COLS) * (tile_h + TG);
                    int down = cur && pressed == -2 && down_x >= x && down_x < x + tile_w && down_y >= y && down_y < y + tile_h;
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
