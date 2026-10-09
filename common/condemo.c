// condemo.c - text console demo for iOS and Android framebuffers.
// usage: condemo [seconds] [scale]    (defaults 15, 2; scale multiplies the 8x16 font)
#include "plat.h"
#include "console.h"
#include <stdlib.h>
#include <sys/time.h>

static double now(void) {
    struct timeval tv; gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

int main(int argc, char **argv) {
    double seconds = argc > 1 ? atof(argv[1]) : 15;
    int scale = argc > 2 ? atoi(argv[2]) : 2;
    PlatInfo pi;
    if (plat_init(&pi)) return 1;
    Con c;
    if (con_init(&c, pi.w, pi.h, scale)) return 1;
    c.rs = pi.rs; c.gs = pi.gs; c.bs = pi.bs; c.alpha = pi.alpha;

    con_color(&c, 0xFFFF55);
    con_printf(&c, "framebuffer text console\n");
    con_color(&c, 0xCCCCCC);
    con_printf(&c, "%s\n%dx%d px, %dx%d cells (8x16 font, %dx)\n\n", pi.name, pi.w, pi.h, c.cols, c.rows, scale);

    static const struct { const char *name; uint32_t rgb; } pal[] = {
        {"red", 0xFF5555}, {"green", 0x55FF55}, {"blue", 0x5599FF}, {"yellow", 0xFFFF55},
        {"cyan", 0x55FFFF}, {"magenta", 0xFF55FF}, {"white", 0xFFFFFF}, {"gray", 0x888888},
    };
    for (int i = 0; i < 8; i++) { con_color(&c, pal[i].rgb); con_printf(&c, "%s ", pal[i].name); }
    con_puts(&c, "\n\n");

    con_color(&c, 0xAAAAAA);
    for (int ch = 32; ch < 127; ch++) {
        con_putc(&c, ch);
        if ((ch - 31) % 32 == 0) con_putc(&c, '\n');
    }
    con_puts(&c, "\n\n");

    double t0 = now(), next_line = t0 + 1.0;
    int frame = 0, tick = 0;
    while (now() - t0 < seconds) {
        double t = now();
        if (t >= next_line) {
            con_color(&c, pal[tick % 8].rgb);
            con_printf(&c, "[%6.2f] tick %d, frame %d\n", t - t0, tick, frame);
            tick++;
            next_line += 0.2;
        }
        int stride;
        uint32_t *px = plat_begin(&stride);
        con_render(&c, px, stride, pi.w, pi.h, ((int)((t - t0) * 2)) & 1);   // 2 Hz cursor blink
        plat_flip();
        frame++;
    }
    double dt = now() - t0;
    plat_close();
    printf("%d frames in %.2fs = %.1f fps\n", frame, dt, frame / dt);
    return 0;
}
