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
    { "Calibrate", "fix touch axes",      "+",   0x6E3AA5, BASE "/calib",                 K_RUN },
#else
    { "Paint",     "draw with fingers",   "~",   0x3A6EA5, BASE "/paint 3600",            K_RUN },
    { "Shell",     "terminal + keyboard", ">_",  0x3AA56E, BASE "/fbshell 3600 2",        K_RUN },
    { "Console",   "text demo",           "Aa",  0xA58A3A, BASE "/condemo 20 2",          K_RUN },
#endif
#ifdef __APPLE__
    { "Shut down", "turn the phone off",  "o|",  0x6A2A2A, "/sbin/halt",                  K_SHUTDOWN },
#endif
    { "Exit",      "back to the phone",   "x",   0x883333, NULL,                          K_QUIT },
};
#define N_ALL ((int)(sizeof TOOLS / sizeof TOOLS[0]))

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
        if (TOOLS[i].kind == K_QUIT) { if (!kiosk) shown[nshown++] = i; continue; }
        if (kiosk && strstr(TOOLS[i].cmd, "/calib")) continue;               // a bad calibration would lock the user out
        char buf[512]; snprintf(buf, sizeof buf, "%s", TOOLS[i].cmd);
        char *sp = strchr(buf, ' '); if (sp) *sp = 0;
        if (access(buf, X_OK) == 0) shown[nshown++] = i;
    }

    const int GAP = 20, COLS = 2, HDR = 130;
    const int tile_w = (W - (COLS + 1) * GAP) / COLS;
    const int rows = (nshown + COLS - 1) / COLS;
    int tile_h = (H - HDR - GAP * (rows + 1)) / (rows ? rows : 1);
    if (tile_h > 210) tile_h = 210;

    enum { S_MAIN, S_CARTS, S_SHUT } state = S_MAIN;
    int scroll = 0, dirty = 3, pressed = -1;
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
        if (tap && state == S_MAIN) {
            for (int k = 0; k < nshown; k++) {
                int x = GAP + (k % COLS) * (tile_w + GAP), y = HDR + GAP + (k / COLS) * (tile_h + GAP);
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
                Surf s = { px, stride, W, H, pi.rs, pi.gs, pi.bs, pi.alpha };
                draw_rect(&s, 0, 0, W, H, 0x000000);
                draw_text(&s, GAP + 6, H / 2 - 16, 3, "Shutting down...", 0xFFFFFF);
                plat_flip();
                sync(); system("/sbin/halt");
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

        if (dirty > 0) {
            int stride; uint32_t *px = plat_begin(&stride);
            Surf s = { px, stride, W, H, pi.rs, pi.gs, pi.bs, pi.alpha };
            draw_rect(&s, 0, 0, W, H, 0x101820);
            if (state == S_MAIN) {
                draw_text(&s, GAP + 6, 26, 4, "FB TOOLS", 0xFFFFFF);
                draw_text(&s, GAP + 6, 88, 2, DEVICE_NAME ", drawing straight to the screen", 0x7788AA);
                for (int k = 0; k < nshown; k++) {
                    const Tool *t = &TOOLS[shown[k]];
                    int x = GAP + (k % COLS) * (tile_w + GAP), y = HDR + GAP + (k / COLS) * (tile_h + GAP);
                    draw_rect(&s, x, y, tile_w, tile_h, t->color);
                    draw_rect(&s, x, y + tile_h - 6, tile_w, 6, 0x000000);
                    draw_text(&s, x + 20, y + 18, 5, t->glyph, 0xFFFFFF);
                    draw_text(&s, x + 20, y + tile_h - 78, 3, t->label, 0xFFFFFF);
                    draw_text(&s, x + 20, y + tile_h - 30, 1, t->sub, 0xDDDDDD);
                }
                if (cur && pressed == -2) {                                          // highlight the tile under the finger
                    for (int k = 0; k < nshown; k++) {
                        int x = GAP + (k % COLS) * (tile_w + GAP), y = HDR + GAP + (k / COLS) * (tile_h + GAP);
                        if (down_x >= x && down_x < x + tile_w && down_y >= y && down_y < y + tile_h) {
                            draw_rect(&s, x, y, tile_w, 8, 0xFFFFFF); draw_rect(&s, x, y + tile_h - 8, tile_w, 8, 0xFFFFFF);
                            draw_rect(&s, x, y, 8, tile_h, 0xFFFFFF); draw_rect(&s, x + tile_w - 8, y, 8, tile_h, 0xFFFFFF);
                        }
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
