// calib.c - touch axis calibration for iOS. Tap the marker at TL, TR, BR; writes /var/root/mt_xform.
#include "plat.h"
#include "draw.h"
#include <math.h>
#include <stdlib.h>
#include <sys/time.h>
#include <unistd.h>

static double now(void) { struct timeval tv; gettimeofday(&tv, NULL); return tv.tv_sec + tv.tv_usec / 1e6; }

static void show(const PlatInfo *pi, int cx, int cy, const char *label, int step) {
    int stride; uint32_t *px = plat_begin(&stride);
    Surf s = { px, stride, pi->w, pi->h, pi->rs, pi->gs, pi->bs, pi->alpha };
    draw_rect(&s, 0, 0, pi->w, pi->h, 0x101820);
    draw_disk(&s, cx, cy, 44, 0xFFAA22);
    draw_disk(&s, cx, cy, 14, 0x101820);
    draw_text(&s, 40, pi->h / 2 - 40, 2, "TAP THE ORANGE DOT", 0xFFFFFF);
    draw_text(&s, 40, pi->h / 2, 2, label, 0x88CCFF);
    char b[32]; snprintf(b, sizeof b, "step %d of 3", step);
    draw_text(&s, 40, pi->h / 2 + 50, 2, b, 0xAAAAAA);
    plat_flip();
}

// Wait for a touch on the current screen, average the raw position while held, wait for release.
static int capture(const PlatInfo *pi, int cx, int cy, const char *label, int step, float *ox, float *oy) {
    double t0 = now();
    float sx = 0, sy = 0; int n = 0, down = 0;
    for (;;) {
        show(pi, cx, cy, label, step);
        float x, y;
        int d = plat_mt_raw(&x, &y);
        if (d) { down = 1; sx += x; sy += y; n++; }
        else if (down) break;                                  // released
        if (now() - t0 > 60) return -1;
    }
    if (n < 3) return capture(pi, cx, cy, label, step, ox, oy);
    *ox = sx / n; *oy = sy / n;
    return 0;
}

int main(void) {
    PlatInfo pi;
    if (plat_init(&pi) || plat_touch_init(&pi)) return 1;
    int m = 70;
    float tl[2], tr[2], br[2];
    if (capture(&pi, m, m, "TOP-LEFT corner", 1, &tl[0], &tl[1])) return 1;
    if (capture(&pi, pi.w - m, m, "TOP-RIGHT corner", 2, &tr[0], &tr[1])) return 1;
    if (capture(&pi, pi.w - m, pi.h - m, "BOTTOM-RIGHT corner", 3, &br[0], &br[1])) return 1;

    float dxx = tr[0] - tl[0], dxy = tr[1] - tl[1];        // raw movement for screen +x
    float dyx = br[0] - tr[0], dyy = br[1] - tr[1];        // raw movement for screen +y
    int swap = fabsf(dxx) < fabsf(dxy);
    int flipx = swap ? dxy < 0 : dxx < 0;
    int flipy = swap ? dyx < 0 : dyy < 0;
    int xform = (flipx ? 1 : 0) | (flipy ? 2 : 0) | (swap ? 4 : 0);
    fprintf(stderr, "raw TL=(%.3f,%.3f) TR=(%.3f,%.3f) BR=(%.3f,%.3f)\n", tl[0], tl[1], tr[0], tr[1], br[0], br[1]);
    fprintf(stderr, "screen +x moves raw (%+.3f,%+.3f); screen +y moves raw (%+.3f,%+.3f)\n", dxx, dxy, dyx, dyy);
    fprintf(stderr, "swap=%d flipx=%d flipy=%d -> xform %d\n", swap, flipx, flipy, xform);
    FILE *f = fopen("/var/root/mt_xform", "w");
    if (f) { fprintf(f, "%d\n", xform); fclose(f); }

    int stride; uint32_t *px = plat_begin(&stride);
    Surf s = { px, stride, pi.w, pi.h, pi.rs, pi.gs, pi.bs, pi.alpha };
    draw_rect(&s, 0, 0, pi.w, pi.h, 0x103018);
    draw_text(&s, 40, pi.h / 2 - 20, 2, "CALIBRATED", 0xFFFFFF);
    char b[48]; snprintf(b, sizeof b, "xform %d", xform);
    draw_text(&s, 40, pi.h / 2 + 30, 2, b, 0x88FF88);
    plat_flip(); usleep(2500000);
    plat_close();
    return 0;
}
