// player.c - TIC-80 on the raw framebuffer (iOS 7 / Android). Core + Lua only, no studio.
// usage: tic80 <cart.tic | cart.lua> [seconds]     (default 3600)
// Game is scaled by an integer factor at the top; D-pad, A/B/X/Y and EXIT are drawn below.
// Touching the game area acts as the mouse. Sound plays through an AudioQueue on iOS (see audio.h).
#include "../common/plat.h"
#include "../common/draw.h"
#include "audio.h"
#include "tic80.h"
#include "cart.h"                 // internal: tic_cartridge, tic_cart_save
#include "studio/project.h"       // internal: tic_project_load (reads -- <TILES> etc. sections from .lua)
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#ifdef __ANDROID__
// The static Android binary has no dynamic loader. The core only calls these to look for external
// script modules, and Lua is compiled in, so "not found" is always the right answer.
void *dlopen(const char *name, int flags) { (void)name; (void)flags; return NULL; }
void *dlsym(void *h, const char *name) { (void)h; (void)name; return NULL; }
int dlclose(void *h) { (void)h; return 0; }
#endif

static int g_quit;
static long g_snd_frames, g_snd_loud_ticks;     // audio statistics, printed at exit
static int g_snd_peak, g_snd_first_count = -1;

static double now(void) { struct timeval tv; gettimeofday(&tv, NULL); return tv.tv_sec + tv.tv_usec / 1e6; }
static u64 counter_cb(void *d) { (void)d; return (u64)(now() * 1000000.0); }
static u64 freq_cb(void *d) { (void)d; return 1000000; }
static void trace_cb(const char *text, u8 color) { (void)color; fprintf(stderr, "trace: %s\n", text); }
static void error_cb(const char *info) { fprintf(stderr, "error: %s\n", info); }
static void exit_cb(void) { g_quit = 1; }

static unsigned char *read_file(const char *path, long *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); *size = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *buf = malloc(*size + 1);
    if (buf && fread(buf, 1, *size, f) != (size_t)*size) { free(buf); buf = NULL; }
    if (buf) buf[*size] = 0;
    fclose(f);
    return buf;
}

// Load a text project (.lua with -- <TILES> / <SPRITES> / <MAP> / <PALETTE> / <WAVES> / ... sections)
// through the core's project parser, then serialize it to the binary cart format tic80_load() expects.
static unsigned char *project_to_cart(const char *name, const unsigned char *src, long n, long *out_size) {
    tic_cartridge *cart = calloc(1, sizeof(tic_cartridge));
    if (!cart || !tic_project_load(name, (const char *)src, (s32)n, cart)) { fprintf(stderr, "cannot parse project %s\n", name); free(cart); return NULL; }
    unsigned char *buf = malloc(sizeof(tic_cartridge) * 2);
    *out_size = tic_cart_save(cart, buf);
    free(cart);
    return buf;
}

static int in_circle(int x, int y, int cx, int cy, int r) { int dx = x - cx, dy = y - cy; return dx * dx + dy * dy <= r * r; }

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: tic80 <cart.tic|cart.lua> [seconds]\n"); return 2; }
    double seconds = argc > 2 ? atof(argv[2]) : 3600;

    long size = 0;
    unsigned char *data = read_file(argv[1], &size);
    if (!data) { fprintf(stderr, "cannot read %s\n", argv[1]); return 1; }
    size_t pl = strlen(argv[1]);
    if (pl > 4 && !strcmp(argv[1] + pl - 4, ".lua")) {
        long wsize; unsigned char *w = project_to_cart(argv[1], data, size, &wsize);
        if (!w) return 1;
        free(data); data = w; size = wsize;
    }

    PlatInfo pi;
    if (plat_init(&pi) || plat_touch_init(&pi)) return 1;
    int W = pi.w, H = pi.h;
    int scale = W / TIC80_FULLWIDTH; if (scale < 1) scale = 1;
    int gw = TIC80_FULLWIDTH * scale, gh = TIC80_FULLHEIGHT * scale;
    int gx = (W - gw) / 2, gy = 8;

    // controls layout (below the game)
    int cy = gy + gh + (H - gy - gh) / 2, avail = H - gy - gh;
    int R = (W * 20 / 100 < avail * 38 / 100) ? W * 20 / 100 : avail * 38 / 100;     // d-pad radius
    int dcx = W * 24 / 100, bcx = W * 76 / 100, S = R * 62 / 100, br = R * 38 / 100;
    int exit_w = 130, exit_h = 52;

    tic80 *tic = tic80_create(TIC80_SAMPLERATE, TIC80_PIXEL_COLOR_ARGB8888);
    tic->callback.trace = trace_cb; tic->callback.error = error_cb; tic->callback.exit = exit_cb;
    tic80_load(tic, data, (s32)size);
    if (snd_init(TIC80_SAMPLERATE)) fprintf(stderr, "no sound output on this platform\n");

    int prev_down[16] = {0};
    double t0 = now(), last = t0, acc = 0;
    long frames = 0, ticks = 0;
    tic80_input input;

    while (!g_quit && now() - t0 < seconds) {
        PlatTouch t[10];
        int n = plat_touch_poll(t, 10);
        int cur_down[16] = {0};
        int up = 0, down = 0, left = 0, right = 0, a = 0, b = 0, x = 0, y = 0;
        int mouse = 0, mx = 0, my = 0;
        for (int i = 0; i < n; i++) {
            int tx = t[i].x, ty = t[i].y; cur_down[t[i].id & 15] = 1;
            if (ty >= gy && ty < gy + gh && tx >= gx && tx < gx + gw) {                  // game area = mouse
                mouse = 1;
                mx = (tx - gx - TIC80_MARGIN_LEFT * scale) / scale; my = (ty - gy - TIC80_MARGIN_TOP * scale) / scale;
                if (mx < 0) mx = 0; if (mx > TIC80_WIDTH - 1) mx = TIC80_WIDTH - 1;
                if (my < 0) my = 0; if (my > TIC80_HEIGHT - 1) my = TIC80_HEIGHT - 1;
                continue;
            }
            if (!prev_down[t[i].id & 15] && tx < exit_w && ty >= H - exit_h) { g_quit = 1; continue; }
            if (in_circle(tx, ty, dcx, cy, R * 13 / 10)) {
                int dx = tx - dcx, dy = ty - cy, th = R * 3 / 10;
                if (dx > th) right = 1; if (dx < -th) left = 1; if (dy > th) down = 1; if (dy < -th) up = 1;
            }
            if (in_circle(tx, ty, bcx, cy - S, br * 12 / 10)) x = 1;
            if (in_circle(tx, ty, bcx - S, cy, br * 12 / 10)) y = 1;
            if (in_circle(tx, ty, bcx + S, cy, br * 12 / 10)) a = 1;
            if (in_circle(tx, ty, bcx, cy + S, br * 12 / 10)) b = 1;
        }
        for (int i = 0; i < 16; i++) prev_down[i] = cur_down[i];

        memset(&input, 0, sizeof input);
        input.gamepads.first.up = up; input.gamepads.first.down = down;
        input.gamepads.first.left = left; input.gamepads.first.right = right;
        input.gamepads.first.a = a; input.gamepads.first.b = b;
        input.gamepads.first.x = x; input.gamepads.first.y = y;
        if (mouse) { input.mouse.x = (u8)mx; input.mouse.y = (u8)my; input.mouse.left = 1; }
        else { input.mouse.x = (u8)mx; input.mouse.y = (u8)my; }

        double tn = now(); acc += tn - last; last = tn;
        if (acc > 0.1) acc = 0.1;
        int nt = 0;
        while (acc >= 1.0 / 60 && nt < 4) {
            tic80_tick(tic, input, counter_cb, freq_cb);
            tic80_sound(tic);                                   // synthesize this tick's audio, then queue it
            snd_push(tic->samples.buffer, tic->samples.count / TIC80_SAMPLE_CHANNELS);
            if (g_snd_first_count < 0) g_snd_first_count = tic->samples.count;
            int peak = 0;
            for (int k = 0; k < tic->samples.count; k++) { int v = tic->samples.buffer[k]; if (v < 0) v = -v; if (v > peak) peak = v; }
            if (peak > g_snd_peak) g_snd_peak = peak;
            if (peak > 0) g_snd_loud_ticks++;
            g_snd_frames += tic->samples.count / TIC80_SAMPLE_CHANNELS;
            acc -= 1.0 / 60; nt++; ticks++;
        }
        if (nt == 0 && frames > 0) { usleep(1000); continue; }

        int stride; uint32_t *px = plat_begin(&stride);
        Surf s = { px, stride, W, H, pi.rs, pi.gs, pi.bs, pi.alpha };
        draw_rect(&s, 0, 0, W, H, 0x14181F);

        // game: convert ARGB8888 to the framebuffer's channel order, scale by an integer factor
        int ident = pi.rs == 16 && pi.gs == 8 && pi.bs == 0;
        for (int sy = 0; sy < TIC80_FULLHEIGHT; sy++) {
            uint32_t *line = px + (size_t)(gy + sy * scale) * stride + gx;
            const uint32_t *src = (const uint32_t *)tic->screen + sy * TIC80_FULLWIDTH;
            for (int sx = 0; sx < TIC80_FULLWIDTH; sx++) {
                uint32_t p = src[sx];
                uint32_t v = ident ? (p | pi.alpha)
                                   : (((p >> 16 & 0xFF) << pi.rs) | ((p >> 8 & 0xFF) << pi.gs) | ((p & 0xFF) << pi.bs) | pi.alpha);
                for (int k = 0; k < scale; k++) line[sx * scale + k] = v;
            }
            for (int k = 1; k < scale; k++) memcpy(line + (size_t)k * stride, line, (size_t)gw * sizeof(uint32_t));
        }

        // controls
        draw_disk(&s, dcx, cy, R, 0x232A36);
        draw_rect(&s, dcx - R * 8 / 100, cy - R * 85 / 100, R * 16 / 100, R * 170 / 100, 0x2E3748);
        draw_rect(&s, dcx - R * 85 / 100, cy - R * 8 / 100, R * 170 / 100, R * 16 / 100, 0x2E3748);
        uint32_t on = 0x5A80B8;
        if (up)    draw_disk(&s, dcx, cy - R * 55 / 100, R * 20 / 100, on);
        if (down)  draw_disk(&s, dcx, cy + R * 55 / 100, R * 20 / 100, on);
        if (left)  draw_disk(&s, dcx - R * 55 / 100, cy, R * 20 / 100, on);
        if (right) draw_disk(&s, dcx + R * 55 / 100, cy, R * 20 / 100, on);
        struct { int cx, cy, p; const char *l; uint32_t c; } bt[4] = {
            { bcx, cy - S, x, "X", 0x3A6EA5 }, { bcx - S, cy, y, "Y", 0x3AA56E },
            { bcx + S, cy, a, "A", 0xA53A4E }, { bcx, cy + S, b, "B", 0xA58A3A } };
        for (int i = 0; i < 4; i++) {
            draw_disk(&s, bt[i].cx, bt[i].cy, br, bt[i].p ? 0xFFFFFF : bt[i].c);
            draw_text(&s, bt[i].cx - 8 * 2 / 1 / 2 * 1 - 0, bt[i].cy - 16, 2, bt[i].l, bt[i].p ? 0x101010 : 0xFFFFFF);
        }
        draw_rect(&s, 0, H - exit_h, exit_w, exit_h, 0x883333);
        draw_text(&s, 28, H - exit_h + 10, 2, "EXIT", 0xFFFFFF);

        plat_flip();
        frames++;
    }
    double dt = now() - t0;
    snd_close();
    plat_close();
    fprintf(stderr, "%ld frames, %ld ticks in %.1fs (%.1f fps display, %.1f tick/s)\n", frames, ticks, dt, frames / dt, ticks / dt);
    fprintf(stderr, "audio: %ld frames made, %d samples per tick, %ld ticks with sound, loudest sample %d of 32767\n",
            g_snd_frames, g_snd_first_count, g_snd_loud_ticks, g_snd_peak);
    return 0;
}
