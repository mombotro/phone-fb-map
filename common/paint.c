// paint.c - multitouch paint demo for iOS and Android framebuffers.
// usage: paint [seconds]    (default 60). Tap EXIT (top-left) to quit, CLEAR (top-right) to wipe.
#include "plat.h"
#include "draw.h"
#include <stdlib.h>
#include <sys/time.h>

#define UI_H 48
#define BRUSH 8

static double now(void) {
    struct timeval tv; gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

int main(int argc, char **argv) {
    double seconds = argc > 1 ? atof(argv[1]) : 60;
    PlatInfo pi;
    if (plat_init(&pi)) return 1;
    if (plat_touch_init(&pi)) return 1;
    int W = pi.w, H = pi.h;

    static const uint32_t pal[8] = {0xFF5555, 0x55FF55, 0x5599FF, 0xFFFF55, 0x55FFFF, 0xFF55FF, 0xFFFFFF, 0xFF9933};
    uint32_t *canvas = malloc((size_t)W * H * sizeof(uint32_t));
    if (!canvas) return 1;
    Surf cs = { canvas, W, W, H, pi.rs, pi.gs, pi.bs, pi.alpha };
    draw_rect(&cs, 0, 0, W, H, 0x101820);

    int prev_active[16] = {0}, lastx[16], lasty[16];
    int frames = 0, quit = 0;
    double t0 = now(), tfps = t0, fps = 0;
    int tx = -1, ty = -1, tn = 0;

    while (!quit && now() - t0 < seconds) {
        PlatTouch t[10];
        int n = plat_touch_poll(t, 10);
        int cur_active[16] = {0};
        for (int i = 0; i < n; i++) {
            int id = t[i].id & 15, x = t[i].x, y = t[i].y;
            cur_active[id] = 1;
            tx = x; ty = y;
            if (!prev_active[id] && y < UI_H) {                      // new touch on the button bar
                if (x < 130) quit = 1;
                else if (x > W - 150) draw_rect(&cs, 0, 0, W, H, 0x101820);
                continue;
            }
            if (y < UI_H) continue;                                  // dragging along the bar: no painting
            if (prev_active[id]) draw_line(&cs, lastx[id], lasty[id], x, y, BRUSH, pal[id & 7]);
            else draw_disk(&cs, x, y, BRUSH, pal[id & 7]);
            lastx[id] = x; lasty[id] = y;
        }
        for (int i = 0; i < 16; i++) prev_active[i] = cur_active[i];
        tn = n;

        int stride;
        uint32_t *px = plat_begin(&stride);
        for (int y = 0; y < H; y++) memcpy(px + (size_t)y * stride, canvas + (size_t)y * W, (size_t)W * sizeof(uint32_t));
        Surf s = { px, stride, W, H, pi.rs, pi.gs, pi.bs, pi.alpha };
        draw_rect(&s, 0, 0, W, UI_H, 0x303848);
        draw_rect(&s, 0, 0, 130, UI_H, 0x883333);
        draw_text(&s, 12, 8, 2, "EXIT", 0xFFFFFF);
        draw_rect(&s, W - 150, 0, 150, UI_H, 0x335588);
        draw_text(&s, W - 138, 8, 2, "CLEAR", 0xFFFFFF);
        char buf[96];
        snprintf(buf, sizeof buf, "n=%d x=%d y=%d %.0ffps", tn, tx, ty, fps);
        draw_text(&s, 140, 16, 1, buf, 0xCCCCCC);
        for (int i = 0; i < n; i++) {                                // crosshair under each finger
            draw_rect(&s, t[i].x - 20, t[i].y, 40, 1, 0xFFFFFF);
            draw_rect(&s, t[i].x, t[i].y - 20, 1, 40, 0xFFFFFF);
        }
        plat_flip();

        frames++;
        double tn_ = now();
        if (tn_ - tfps >= 0.5) { fps = frames / (tn_ - t0); tfps = tn_; }
    }
    plat_close();
    printf("%d frames in %.2fs\n", frames, now() - t0);
    return 0;
}
