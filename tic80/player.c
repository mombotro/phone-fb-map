// player.c - TIC-80 on the raw framebuffer (iOS 7 / Android). Core + Lua and Fennel, no studio.
// usage: tic80 <cart.tic | cart.lua> [seconds]     (default 3600)
// Portrait: the game as wide as the screen with the controls below. Landscape: the game as tall as the screen with see-through controls over its sides.
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

// Draw with a see-through look: alpha 255 is solid, 0 is invisible. The pixels under the shape keep their share.
static uint32_t mix_px(const Surf *s, uint32_t under, uint32_t rgb, int a) {
    uint32_t out = s->alpha;
    int sh[3] = { s->rs, s->gs, s->bs }, cs[3] = { 16, 8, 0 };
    for (int k = 0; k < 3; k++) {
        int u = (int)((under >> sh[k]) & 0xFF), c = (int)((rgb >> cs[k]) & 0xFF);
        out |= (uint32_t)((u * (255 - a) + c * a) / 255) << sh[k];
    }
    return out;
}
static void pill(Surf *s, int x, int y, int w, int h, uint32_t rgb) {                 // a bar with round ends
    draw_rect(s, x + h / 2, y, w - h, h, rgb); draw_disk(s, x + h / 2, y + h / 2, h / 2, rgb); draw_disk(s, x + w - h / 2, y + h / 2, h / 2, rgb);
}
static void sdisk(Surf *s, int cx, int cy, int r, uint32_t rgb, int a) {
    if (a >= 255) { draw_disk(s, cx, cy, r, rgb); return; }
    for (int y = cy - r; y <= cy + r; y++) {
        if (y < 0 || y >= s->h) continue;
        int dy = y - cy, w2 = r * r - dy * dy; if (w2 < 0) continue;
        for (int x = cx - r; x <= cx + r; x++) {
            if (x < 0 || x >= s->w) continue;
            int dx = x - cx; if (dx * dx > w2) continue;
            uint32_t *p = s->px + (size_t)y * s->stride + x; *p = mix_px(s, *p, rgb, a);
        }
    }
}
static void srect(Surf *s, int x0, int y0, int w, int h, uint32_t rgb, int a) {
    if (a >= 255) { draw_rect(s, x0, y0, w, h, rgb); return; }
    for (int y = y0 < 0 ? 0 : y0; y < y0 + h && y < s->h; y++)
        for (int x = x0 < 0 ? 0 : x0; x < x0 + w && x < s->w; x++) { uint32_t *p = s->px + (size_t)y * s->stride + x; *p = mix_px(s, *p, rgb, a); }
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: tic80 <cart.tic|cart.lua> [seconds]\n"); return 2; }
    double seconds = argc > 2 ? atof(argv[2]) : 3600;

    long size = 0;
    unsigned char *data = read_file(argv[1], &size);
    if (!data) { fprintf(stderr, "cannot read %s\n", argv[1]); return 1; }
    size_t pl = strlen(argv[1]);
    if (!(pl > 4 && !strcmp(argv[1] + pl - 4, ".tic"))) {                                  // a project file in any language: .lua .js .moon .fnl ...
        long wsize; unsigned char *w = project_to_cart(argv[1], data, size, &wsize);
        if (!w) return 1;
        free(data); data = w; size = wsize;
    }

    PlatInfo pi;
    if (plat_init(&pi) || plat_touch_init(&pi)) return 1;
    int W = pi.w, H = pi.h;
    // Portrait: the game on top, as wide as the screen, and the controls below. Landscape (wider than tall): the game in the middle, just as big
    // as in portrait (as wide as the short side of the screen), the d-pad on its left and the buttons on its right. The picture keeps its shape,
    // so the scale is not a whole number (every game pixel is one or more screen pixels, some of them one more than the others).
    int land = W > H;
    int gw, gh, gx, gy, cy, R, dcx, bcx, S, br, exit_x, exit_y;
    int exit_w = 130, exit_h = 52;
    int over = 0;                                                                // 1: the controls lie over the game (not used now)
    if (land) {                                                                  // the same size as in portrait (as wide as the short side), controls beside it
        gw = H; if (gw > W * 60 / 100) gw = W * 60 / 100;
        gh = gw * TIC80_FULLHEIGHT / TIC80_FULLWIDTH;
        gx = (W - gw) / 2; gy = (H - gh) / 2; cy = H / 2;
        R = gx * 42 / 100 < H * 26 / 100 ? gx * 42 / 100 : H * 26 / 100;           // d-pad radius
        dcx = gx / 2; bcx = W - gx / 2; S = R * 62 / 100; br = R * 38 / 100;
        exit_w = 200; exit_h = 72; exit_x = (W - exit_w) / 2; exit_y = gy + gh + (H - gy - gh - exit_h) / 2;      // the orange pill under the game
    } else {
        gw = W; gh = W * TIC80_FULLHEIGHT / TIC80_FULLWIDTH;
        gx = 0; gy = 0;
        int pill_y = H - 150, avail = pill_y - (gy + gh); cy = gy + gh + avail / 2;      // the pills (select, exit, start) are in a row at the bottom
        R = (W * 20 / 100 < avail * 38 / 100) ? W * 20 / 100 : avail * 38 / 100;
        dcx = W * 24 / 100; bcx = W * 76 / 100; S = R * 62 / 100; br = R * 38 / 100;
        exit_w = W * 24 / 100; exit_h = 84; exit_x = (W - exit_w) / 2; exit_y = H - 132;           // tall pills: easy to hit
    }
    int *xmap = malloc(sizeof(int) * (size_t)gw), *ymap = malloc(sizeof(int) * (size_t)gh);        // game pixel for each screen column and row
    for (int i = 0; i < gw; i++) xmap[i] = i * TIC80_FULLWIDTH / gw;
    for (int i = 0; i < gh; i++) ymap[i] = i * TIC80_FULLHEIGHT / gh;

    tic80 *tic = tic80_create(TIC80_SAMPLERATE, TIC80_PIXEL_COLOR_BGRA8888);
    tic->callback.trace = trace_cb; tic->callback.error = error_cb; tic->callback.exit = exit_cb;
    tic80_load(tic, data, (s32)size);
    if (snd_init(TIC80_SAMPLERATE)) fprintf(stderr, "no sound output on this platform\n");

    {   // a big cart (a Fennel game) takes seconds to compile on an old phone, in its first tick: say so first
        int stride0; uint32_t *px0 = plat_begin(&stride0);
        Surf s0 = { px0, stride0, W, H, pi.rs, pi.gs, pi.bs, pi.alpha };
        draw_rect(&s0, 0, 0, W, H, 0x14181F);
        draw_text(&s0, gx + (gw - 10 * 16) / 2, gy + gh / 2 - 24, 2, "LOADING...", 0xFFFFFF);
        draw_text(&s0, gx + (gw - 36 * 8) / 2, gy + gh / 2 + 12, 1, "a big game can take a while to start", 0x8899AA);
        plat_flip();
    }
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
            int on_ctl = in_circle(tx, ty, dcx, cy, R * 13 / 10) || in_circle(tx, ty, bcx, cy - S, br * 12 / 10) || in_circle(tx, ty, bcx - S, cy, br * 12 / 10) ||
                         in_circle(tx, ty, bcx + S, cy, br * 12 / 10) || in_circle(tx, ty, bcx, cy + S, br * 12 / 10) ||
                         (tx >= exit_x && tx < exit_x + exit_w && ty >= exit_y && ty < exit_y + exit_h);
            if (!on_ctl && ty >= gy && ty < gy + gh && tx >= gx && tx < gx + gw) {           // game area = mouse (the controls win where they lie over it)
                mouse = 1;
                mx = (tx - gx) * TIC80_FULLWIDTH / gw - TIC80_MARGIN_LEFT; my = (ty - gy) * TIC80_FULLHEIGHT / gh - TIC80_MARGIN_TOP;
                if (mx < 0) mx = 0; if (mx > TIC80_WIDTH - 1) mx = TIC80_WIDTH - 1;
                if (my < 0) my = 0; if (my > TIC80_HEIGHT - 1) my = TIC80_HEIGHT - 1;
                continue;
            }
            if (!prev_down[t[i].id & 15] && tx >= exit_x && tx < exit_x + exit_w && ty >= exit_y && ty < exit_y + exit_h) { g_quit = 1; continue; }
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

        {   // a short buzz when a button or a direction goes down
            static int before[8]; int now8[8] = { up, down, left, right, a, b, x, y }, buzz = 0;
            for (int k = 0; k < 8; k++) { if (now8[k] && !before[k]) buzz = 1; before[k] = now8[k]; }
            if (buzz) plat_rumble(22);
        }
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
        // The body and the controls only change when a button goes down or up. Then they are drawn for a few frames in a row, so that every
        // framebuffer page has them. Other frames draw (and copy to the screen) only the game, which is much less work, most of all in landscape,
        // where the picture has to be turned around in software.
        static unsigned sig_prev = 0xFFFFFFFFu; static int full_left = 0;
        unsigned sig = (unsigned)(up | down << 1 | left << 2 | right << 3 | a << 4 | b << 5 | x << 6 | y << 7);
        if (sig != sig_prev) { sig_prev = sig; full_left = 4; }
        int full = full_left > 0; if (full_left > 0) full_left--;
        if (full) draw_rect(&s, 0, 0, W, H, land ? 0xC9C0AC : 0x14181F);                // landscape: the beige body behind everything

        // game: the core writes bytes B,G,R,A, which is 0xAARRGGBB as a number here. Convert it to the framebuffer's channel order and
        // scale it to the area gw by gh (nearest pixel). A screen row is made once, and the rows that show the same game row copy it.
        int ident = pi.rs == 16 && pi.gs == 8 && pi.bs == 0;
        for (int dy = 0; dy < gh; dy++) {
            uint32_t *line = px + (size_t)(gy + dy) * stride + gx;
            if (dy > 0 && ymap[dy] == ymap[dy - 1]) { memcpy(line, line - stride, (size_t)gw * sizeof(uint32_t)); continue; }
            const uint32_t *src = (const uint32_t *)tic->screen + ymap[dy] * TIC80_FULLWIDTH;
            for (int dx = 0; dx < gw; dx++) {
                uint32_t p = src[xmap[dx]];
                line[dx] = ident ? (p | pi.alpha)
                                 : (((p >> 16 & 0xFF) << pi.rs) | ((p >> 8 & 0xFF) << pi.gs) | ((p & 0xFF) << pi.bs) | pi.alpha);
            }
        }

        if (full) {
        // controls, in the look of a small handheld: a beige body, a dark rounded d-pad, four dark red buttons, gray pills and an orange one
        const int A = over ? 150 : 255;                                          // how solid they are (0 to 255): see-through over the game
        const uint32_t BODY = 0xC9C0AC, BODY_DK = 0xAFA590, PAD = 0x5F5C59, PAD_EDGE = 0x3F3D3B, PAD_LIT = 0x8A8682;
        const uint32_t RED = 0x8E2634, RED_ON = 0xC24A58, ORANGE = 0xF29B38, LETTER = 0xEADDD0;
        if (!over && !land) {
            int py0 = gy + gh;
            draw_rect(&s, 0, py0, W, H - py0, BODY); draw_rect(&s, 0, py0, W, 3, BODY_DK);
            for (int i = 0; i < 3; i++) draw_rect(&s, W * 6 / 100, py0 + 14 + i * 9, W * 9 / 100, 3, BODY_DK);          // speaker slots
        }
        {   // the d-pad: a plus of two arms with rounded ends
            int ah = R * 31 / 100, al = R * 95 / 100;                            // half width and half length of an arm
            if (A >= 255) {
                for (int e = 0; e < 2; e++) {
                    int g = e ? 0 : 4; uint32_t c = e ? PAD : PAD_EDGE;          // edge first, then the face
                    int rr = ah * 40 / 100;
                    draw_rect(&s, dcx - al - g + rr, cy - ah - g, 2 * (al + g) - 2 * rr, 2 * (ah + g), c);
                    draw_rect(&s, dcx - al - g, cy - ah - g + rr, 2 * (al + g), 2 * (ah + g) - 2 * rr, c);
                    draw_rect(&s, dcx - ah - g + rr, cy - al - g, 2 * (ah + g) - 2 * rr, 2 * (al + g), c);
                    draw_rect(&s, dcx - ah - g, cy - al - g + rr, 2 * (ah + g), 2 * (al + g) - 2 * rr, c);
                    int sx[4] = { -1, 1, -1, 1 }, sy[4] = { -1, -1, 1, 1 };
                    for (int k = 0; k < 4; k++) {                                // the four ends of the arms: round corners
                        draw_disk(&s, dcx + sx[k] * (al + g - rr), cy + sy[k] * (ah + g - rr), rr, c);
                        draw_disk(&s, dcx + sx[k] * (ah + g - rr), cy + sy[k] * (al + g - rr), rr, c);
                    }
                }
            } else {                                                             // see-through: plain pieces that do not overlap
                srect(&s, dcx - al, cy - ah, 2 * al, 2 * ah, PAD, A);
                srect(&s, dcx - ah, cy - al, 2 * ah, al - ah, PAD, A); srect(&s, dcx - ah, cy + ah, 2 * ah, al - ah, PAD, A);
            }
            int in = ah - 6;                                                     // a pressed direction lights up its whole arm
            if (up)    srect(&s, dcx - in, cy - al + 6, 2 * in, al - ah - 2, PAD_LIT, A);
            if (down)  srect(&s, dcx - in, cy + ah + 2, 2 * in, al - ah - 6, PAD_LIT, A);
            if (left)  srect(&s, dcx - al + 6, cy - in, al - ah - 2, 2 * in, PAD_LIT, A);
            if (right) srect(&s, dcx + ah + 2, cy - in, al - ah - 6, 2 * in, PAD_LIT, A);
            if (A >= 255) draw_disk(&s, dcx, cy, R * 16 / 100, PAD_EDGE);       // the dimple in the middle
        }
        struct { int cx, cy, p; const char *l; } bt[4] = {
            { bcx, cy - S, x, "X" }, { bcx - S, cy, y, "Y" }, { bcx + S, cy, a, "A" }, { bcx, cy + S, b, "B" } };
        static const char *KEY[4] = { "a", "s", "z", "x" };                      // the keyboard keys of TIC-80 for X, Y, A, B (what a game's help text says)
        if (A >= 255) for (int i = 0; i < 4; i++) draw_disk(&s, bt[i].cx, bt[i].cy, br * 128 / 100, BODY_DK);          // the bezel behind each button
        for (int i = 0; i < 4; i++) {                                           // a pressed button is lighter and sinks a little, with a bright rim
            int rr = bt[i].p ? br * 90 / 100 : br;
            if (bt[i].p) sdisk(&s, bt[i].cx, bt[i].cy, br * 106 / 100, 0xF4D8C8, A);
            sdisk(&s, bt[i].cx, bt[i].cy, rr, bt[i].p ? RED_ON : RED, A);
            draw_text(&s, bt[i].cx - 8, bt[i].cy - 20 + (bt[i].p ? 2 : 0), 2, bt[i].l, bt[i].p ? 0xFFFFFF : LETTER);
            draw_text(&s, bt[i].cx - 4, bt[i].cy + 8 + (bt[i].p ? 2 : 0), 1, KEY[i], bt[i].p ? 0xF0D0D0 : 0xC89AA2);
        }
        {   // the orange pill ends the game (TIC-80 has no select or start, so there are no other pills)
            int ph = exit_h;
            if (!land) {
                int py = exit_y;
                draw_text(&s, exit_x + (exit_w - 4 * 8) / 2, py + ph + 8, 1, "EXIT", BODY_DK);
            }
            pill(&s, exit_x, exit_y, exit_w, exit_h, ORANGE);
            if (land) draw_text(&s, exit_x + (exit_w - 4 * 16) / 2, exit_y + (exit_h - 16) / 2, 2, "EXIT", 0x4A2A08);
        }

        }                                                                          // (end of the full frame)
        if (!full) plat_set_dirty(gx, gy, gw, gh);
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
