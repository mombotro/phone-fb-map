// fbdemo.c (Android) - geometry check + animation on /dev/graphics/fb0
// usage: fbdemo [frames] [info]     ("info" = print fb parameters and exit)
// Build static; run as shell (group graphics). To see it, SurfaceFlinger must be stopped (su -c stop).
#include <fcntl.h>
#include <linux/fb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <unistd.h>

#ifndef FBIO_WAITFORVSYNC
#define FBIO_WAITFORVSYNC _IOW('F', 32, uint32_t)
#endif

// Raises the TLS segment alignment to 32; bionic static startup rejects 8 on ARM.
__attribute__((used, aligned(32))) __thread char tls_align_pad[32];

static struct fb_var_screeninfo v;
static struct fb_fix_screeninfo fx;
static uint8_t *base;
static int fd, pages;

static double now(void) {
    struct timeval tv; gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

static uint32_t pack(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)r << v.red.offset) | ((uint32_t)g << v.green.offset) |
           ((uint32_t)b << v.blue.offset);
}

static void rect(uint32_t *page, int x0, int y0, int w, int h, uint32_t c) {
    int stride = fx.line_length / 4;
    for (int y = y0; y < y0 + h; y++) {
        if (y < 0 || y >= (int)v.yres) continue;
        for (int x = x0; x < x0 + w; x++) {
            if (x < 0 || x >= (int)v.xres) continue;
            page[y * stride + x] = c;
        }
    }
}

int main(int argc, char **argv) {
    int frames = argc > 1 ? atoi(argv[1]) : 300;
    int info_only = argc > 2 && !strcmp(argv[2], "info");

    fd = open("/dev/graphics/fb0", O_RDWR);
    if (fd < 0) fd = open("/dev/fb0", O_RDWR);
    if (fd < 0) { perror("open fb0"); return 1; }
    if (ioctl(fd, FBIOGET_VSCREENINFO, &v) || ioctl(fd, FBIOGET_FSCREENINFO, &fx)) {
        perror("ioctl get"); return 1;
    }
    pages = v.yres_virtual / v.yres;
    printf("res %ux%u virt %ux%u bpp %u stride %u smem_len %u pages %d\n",
           v.xres, v.yres, v.xres_virtual, v.yres_virtual, v.bits_per_pixel,
           fx.line_length, fx.smem_len, pages);
    printf("R off %u len %u | G off %u len %u | B off %u len %u | A off %u len %u\n",
           v.red.offset, v.red.length, v.green.offset, v.green.length,
           v.blue.offset, v.blue.length, v.transp.offset, v.transp.length);
    if (info_only) return 0;
    if (v.bits_per_pixel != 32) { fprintf(stderr, "only 32bpp handled\n"); return 1; }
    if (pages < 1) pages = 1;

    base = mmap(NULL, fx.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) { perror("mmap"); return 1; }

    uint32_t white = pack(255, 255, 255);
    int bx = 100, by = 200, dx = 7, dy = 5;
    int page = pages > 1 ? 1 : 0;       // draw to a page that isn't currently shown
    int bad = 0, vs_fail = 0;
    double t0 = now(), last = t0;

    for (int i = 0; i < frames; i++) {
        uint32_t *p = (uint32_t *)(base + (size_t)page * v.yres * fx.line_length);
        int stride = fx.line_length / 4;

        for (uint32_t y = 0; y < v.yres; y++) {          // vertical gradient: black -> blue
            uint32_t c = pack(0, 0, y * 255 / (v.yres - 1));
            for (uint32_t x = 0; x < v.xres; x++) p[y * stride + x] = c;
        }
        rect(p, 0, 0, v.xres, 4, white);                 // 4px border
        rect(p, 0, v.yres - 4, v.xres, 4, white);
        rect(p, 0, 0, 4, v.yres, white);
        rect(p, v.xres - 4, 0, 4, v.yres, white);
        rect(p, 8, 8, 40, 40, pack(255, 0, 0));                          // TL red
        rect(p, v.xres - 48, 8, 40, 40, pack(0, 255, 0));                // TR green
        rect(p, 8, v.yres - 48, 40, 40, pack(255, 255, 0));              // BL yellow
        rect(p, v.xres - 48, v.yres - 48, 40, 40, pack(255, 0, 255));    // BR magenta
        rect(p, bx, by, 80, 80, white);                                  // bouncing box
        bx += dx; by += dy;
        if (bx < 8 || bx > (int)v.xres - 88) dx = -dx;
        if (by < 8 || by > (int)v.yres - 88) dy = -dy;

        struct fb_var_screeninfo pv = v;
        pv.yoffset = page * v.yres;
        if (ioctl(fd, FBIOPAN_DISPLAY, &pv)) bad++;
        uint32_t zero = 0;
        if (ioctl(fd, FBIO_WAITFORVSYNC, &zero)) vs_fail++;
        page = (page + 1) % pages;

        double t = now();                                // cap at ~60 fps if vsync wait is a no-op
        if (t - last < 1.0 / 60) usleep((useconds_t)((1.0 / 60 - (t - last)) * 1e6));
        last = now();
    }
    double dt = now() - t0;
    printf("%d frames in %.2fs = %.1f fps, %d pan errors, %d vsync-ioctl errors\n",
           frames, dt, frames / dt, bad, vs_fail);

    struct fb_var_screeninfo z = v; z.yoffset = 0; ioctl(fd, FBIOPAN_DISPLAY, &z);
    return 0;
}
