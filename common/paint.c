// paint.c - finger painting for the iOS and Android framebuffers.
// usage: paint [seconds] [dir]      (default 60 seconds, dir DATA/paint)
// Top bar: EXIT, UNDO, CLEAR, SAVE, ERASE. Bottom bar: colors and three brush sizes. Every finger paints (multitouch).
// SAVE writes a .bmp file that any computer opens. Undo goes back up to 6 strokes.
#include "plat.h"
#include "draw.h"
#include "kbd.h"
#include "ui.h"
#include "bmp.h"
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifdef __APPLE__
#define DEFAULT_DIR "/var/root/data/paint"
#else
#define DEFAULT_DIR "/data/local/tmp/data/paint"
#endif

#define NUNDO 6
static const uint32_t PAL[10] = { 0x000000, 0xFFFFFF, 0xE03030, 0xF09020, 0xF0E030, 0x30B040, 0x30B0E0, 0x3050E0, 0x9040D0, 0x8B5A2B };
static const int SIZES[3] = { 3, 8, 18 };
#define BG 0xFFFFFF

int main(int argc, char **argv) {
    double seconds = argc > 1 ? atof(argv[1]) : 60;
    char dir[256] = DEFAULT_DIR; if (argc > 2) snprintf(dir, sizeof dir, "%s", argv[2]);
    PlatInfo pi;
    if (plat_init(&pi)) return 1;
    if (plat_touch_init(&pi)) return 1;
    int W = pi.w, H = pi.h;
    const int BAR = 64, PALH = 72, CY0 = BAR, CY1 = H - PALH;       // the canvas is between the bars
    uint32_t *canvas = malloc((size_t)W * H * sizeof(uint32_t));
    uint32_t *undo[NUNDO] = { 0 }; int nundo = 0;
    if (!canvas) return 1;
    for (int i = 0; i < NUNDO; i++) { undo[i] = malloc((size_t)W * H * sizeof(uint32_t)); if (!undo[i]) break; }
    Surf cs = { canvas, W, W, H, pi.rs, pi.gs, pi.bs, pi.alpha, 0 };
    draw_rect(&cs, 0, 0, W, H, BG);

    Ui u; memset(&u, 0, sizeof u);
    int color = 0, size = 1, erase = 0, quit = 0, dirty = 3;
    int active[16] = { 0 }, lastx[16], lasty[16], stroking = 0;
    char msg[96] = ""; double msg_t = 0;
    double t0 = ui_now();

    while (!quit && ui_now() - t0 < seconds) {
        PlatTouch t[10];
        int n = plat_touch_poll(t, 10);
        ui_poll(&u, t, n);
        int now_active[16] = { 0 };
        int any_new_in_canvas = 0;
        for (int i = 0; i < n; i++) {                                // is a new finger starting a stroke?
            int id = t[i].id & 15;
            if (!active[id] && t[i].y >= CY0 && t[i].y < CY1) any_new_in_canvas = 1;
        }
        if (any_new_in_canvas && !stroking && undo[0]) {             // remember the canvas before the stroke
            uint32_t *last = undo[NUNDO - 1];
            for (int k = NUNDO - 1; k > 0; k--) undo[k] = undo[k - 1];
            undo[0] = last; memcpy(undo[0], canvas, (size_t)W * H * sizeof(uint32_t));
            if (nundo < NUNDO) nundo++;
        }
        uint32_t ink = erase ? BG : PAL[color];
        for (int i = 0; i < n; i++) {
            int id = t[i].id & 15, x = t[i].x, y = t[i].y;
            now_active[id] = 1;
            if (!active[id] && (y < CY0 || y >= CY1)) { now_active[id] = 0; continue; }     // a touch that starts on a bar paints nothing
            if (y < CY0 || y >= CY1) { if (y < CY0) y = CY0; else y = CY1 - 1; }
            int r = SIZES[size];
            if (active[id]) draw_line(&cs, lastx[id], lasty[id], x, y, r, ink);
            else draw_disk(&cs, x, y, r, ink);
            lastx[id] = x; lasty[id] = y; dirty = 3;
        }
        stroking = 0;
        for (int i = 0; i < 16; i++) { active[i] = now_active[i]; if (now_active[i]) stroking = 1; }
        if (u.changed || u.tapped) dirty = 3;
        if (msg[0] && ui_now() - msg_t > 2.5) { msg[0] = 0; dirty = 3; }
        if (dirty <= 0) { usleep(4000); continue; }
        dirty--;

        int stride; uint32_t *px = plat_begin(&stride);
        for (int y = 0; y < H; y++) memcpy(px + (size_t)y * stride, canvas + (size_t)y * W, (size_t)W * sizeof(uint32_t));
        Surf s = { px, stride, W, H, pi.rs, pi.gs, pi.bs, pi.alpha, 0 };
        u.s = &s; u.W = W; u.H = H;
        draw_rect(&s, 0, 0, W, BAR, 0x26303E);
        draw_rect(&s, 0, CY1, W, PALH, 0x26303E);
        int bw = (W - 6 * 6) / 5;
        if (ui_button(&u, 6, 8, bw, BAR - 16, "EXIT", 0x883333)) quit = 1;
        if (ui_button(&u, 12 + bw, 8, bw, BAR - 16, "UNDO", 0x34608F) && nundo > 0 && !stroking) {
            memcpy(canvas, undo[0], (size_t)W * H * sizeof(uint32_t));
            uint32_t *first = undo[0]; for (int k = 0; k < NUNDO - 1; k++) undo[k] = undo[k + 1]; undo[NUNDO - 1] = first; nundo--; dirty = 3;
        }
        if (ui_button(&u, 18 + 2 * bw, 8, bw, BAR - 16, "CLEAR", 0x34608F) && !stroking) {
            if (undo[0]) { uint32_t *last = undo[NUNDO - 1]; for (int k = NUNDO - 1; k > 0; k--) undo[k] = undo[k - 1]; undo[0] = last; memcpy(undo[0], canvas, (size_t)W * H * sizeof(uint32_t)); if (nundo < NUNDO) nundo++; }
            draw_rect(&cs, 0, 0, W, H, BG); dirty = 3;
        }
        if (ui_button(&u, 24 + 3 * bw, 8, bw, BAR - 16, "SAVE", 0x2E7D4F) && !stroking) {
            mkdir(dir, 0755);
            char path[320]; snprintf(path, sizeof path, "%s/paint-%ld.bmp", dir, (long)time(NULL));
            int cw = W, ch = CY1 - CY0; uint32_t *rgb = malloc((size_t)cw * ch * sizeof *rgb);
            if (rgb) {
                for (int y = 0; y < ch; y++) for (int x = 0; x < cw; x++) {
                    uint32_t p = canvas[(size_t)(y + CY0) * W + x];
                    rgb[(size_t)y * cw + x] = (((p >> pi.rs) & 255) << 16) | (((p >> pi.gs) & 255) << 8) | ((p >> pi.bs) & 255);
                }
                snprintf(msg, sizeof msg, bmp_save(path, rgb, cw, ch, 1) == 0 ? "Saved %s" : "Save failed: %s", strrchr(path, '/') + 1);
                free(rgb); msg_t = ui_now();
            }
        }
        if (ui_button(&u, 30 + 4 * bw, 8, bw, BAR - 16, erase ? "ERASE*" : "ERASE", erase ? 0xB06020 : 0x34608F) && !stroking) erase = !erase;
        // bottom bar: 10 colors, then 3 brush sizes
        int cells = 13, cw2 = (W - 8) / cells;
        for (int i = 0; i < 10; i++) {
            int x = 4 + i * cw2, y = CY1 + 6;
            draw_rect(&s, x + 2, y, cw2 - 4, PALH - 12, color == i && !erase ? 0xFFFFFF : 0x26303E);
            draw_rect(&s, x + 5, y + 3, cw2 - 10, PALH - 18, PAL[i]);
            if (u.tapped && ui_inside(u.tap_x, u.tap_y, x, CY1, cw2, PALH)) { color = i; erase = 0; dirty = 3; }
        }
        for (int i = 0; i < 3; i++) {
            int x = 4 + (10 + i) * cw2, y = CY1 + 6;
            draw_rect(&s, x + 2, y, cw2 - 4, PALH - 12, size == i ? 0xFFFFFF : 0x4A5568);
            draw_disk(&s, x + cw2 / 2, y + (PALH - 12) / 2, SIZES[i] > 14 ? 14 : SIZES[i], 0x000000);
            if (u.tapped && ui_inside(u.tap_x, u.tap_y, x, CY1, cw2, PALH)) { size = i; dirty = 3; }
        }
        if (msg[0]) { draw_rect(&s, 0, CY1 - 34, W, 34, 0x2E7D4F); draw_text(&s, 12, CY1 - 26, 1, msg, 0xFFFFFF); }
        plat_flip();
    }
    plat_close();
    return 0;
}
