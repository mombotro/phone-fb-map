// fbdemo.c - geometry check + animation for fb.h
// usage: fbdemo [frames]    (default 300)
#include "fb.h"
#include <stdlib.h>
#include <sys/time.h>

static double now(void) {
    struct timeval tv; gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

static void rect(FB *f, int x0, int y0, int w, int h, uint32_t c) {
    for (int y = y0; y < y0 + h; y++) {
        if (y < 0 || y >= (int)f->h) continue;
        for (int x = x0; x < x0 + w; x++) {
            if (x < 0 || x >= (int)f->w) continue;
            f->px[y * (f->stride / 4) + x] = c;
        }
    }
}

int main(int argc, char **argv) {
    int frames = argc > 1 ? atoi(argv[1]) : 300;
    FB f;
    if (fb_init(&f, 640, 960)) return 1;
    f.wait_mode = argc > 2 ? atoi(argv[2]) : 0;

    int bx = 100, by = 200, dx = 7, dy = 5;
    double t0 = now();
    int bad = 0, lastrc = 0;
    for (int i = 0; i < frames; i++) {
        fb_begin(&f);
        // vertical gradient: black at top -> blue at bottom
        for (size_t y = 0; y < f.h; y++) {
            uint32_t c = 0xFF000000u | (uint32_t)(y * 255 / (f.h - 1));
            uint32_t *row = f.px + y * (f.stride / 4);
            for (size_t x = 0; x < f.w; x++) row[x] = c;
        }
        // 4px white border
        rect(&f, 0, 0, (int)f.w, 4, 0xFFFFFFFF);
        rect(&f, 0, (int)f.h - 4, (int)f.w, 4, 0xFFFFFFFF);
        rect(&f, 0, 0, 4, (int)f.h, 0xFFFFFFFF);
        rect(&f, (int)f.w - 4, 0, 4, (int)f.h, 0xFFFFFFFF);
        // corner markers: TL red, TR green, BL yellow, BR magenta (so rotation/mirroring shows)
        rect(&f, 8, 8, 40, 40, 0xFFFF0000);
        rect(&f, (int)f.w - 48, 8, 40, 40, 0xFF00FF00);
        rect(&f, 8, (int)f.h - 48, 40, 40, 0xFFFFFF00);
        rect(&f, (int)f.w - 48, (int)f.h - 48, 40, 40, 0xFFFF00FF);
        // bouncing box
        rect(&f, bx, by, 80, 80, 0xFFFFFFFF);
        bx += dx; by += dy;
        if (bx < 8 || bx > (int)f.w - 88) dx = -dx;
        if (by < 8 || by > (int)f.h - 88) dy = -dy;

        lastrc = fb_flip(&f); if (lastrc != 0) bad++;
    }
    double dt = now() - t0;
    printf("%d frames in %.2fs = %.1f fps, %d bad flips (wait_mode %d, last rc %d)\n", frames, dt, frames / dt, bad, f.wait_mode, lastrc);
    sleep(2);
    return 0;
}
