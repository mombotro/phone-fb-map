// plat_android.h - platform layer over /dev/graphics/fb0 (msm fbdev, 3 pages) and evdev touch.
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
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
// Build with -fno-emulated-tls so this reaches the real TLS segment.
__attribute__((used, aligned(32))) __thread char plat__tls_pad[32];

static struct fb_var_screeninfo plat__v;
static struct fb_fix_screeninfo plat__f;
static uint8_t *plat__base;
static int plat__fd, plat__pages, plat__page;
static double plat__last;
static char plat__bl_path[96];
static int plat__bl_old = -1;

// Best effort: with the framework stopped nobody manages the panel, and a screen that timed out
// before we started stays blank (backlight 0). Unblank it and raise the backlight if it is off.
static void plat__wake_panel(void) {
    // Blank/unblank resets the MDP pipes. Without it a stale SurfaceFlinger overlay or a wedged
    // pipeline can keep the old Android frame on screen while our pans are accepted but invisible.
    ioctl(plat__fd, FBIOBLANK, FB_BLANK_POWERDOWN);
    usleep(300000);
    ioctl(plat__fd, FBIOBLANK, FB_BLANK_UNBLANK);
    static const char *paths[] = { "/sys/class/leds/lcd-backlight/brightness", "/sys/class/leds/backlight/brightness",
                                   "/sys/class/backlight/panel/brightness",
                                   "/sys/class/backlight/lcd-backlight/brightness" };
    for (unsigned i = 0; i < sizeof paths / sizeof paths[0]; i++) {
        int fd = open(paths[i], O_RDWR);
        if (fd < 0) continue;
        char buf[16] = {0};
        int n = read(fd, buf, sizeof buf - 1);
        int old = n > 0 ? atoi(buf) : -1;
        if (old >= 0 && old < 40) {
            lseek(fd, 0, SEEK_SET);
            if (write(fd, "128", 3) == 3) {
                strncpy(plat__bl_path, paths[i], sizeof plat__bl_path - 1);
                plat__bl_old = old;
                fprintf(stderr, "backlight was %d, set to 128 (%s)\n", old, paths[i]);
            }
        }
        close(fd);
        return;
    }
}

static double plat__now(void) {
    struct timeval tv; gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

// Rotation for tablets whose screen is wider than tall (the Kindle Fire is 1024x600). The programs are designed for a tall screen,
// so they get a 600x1024 buffer, and flip rotates it into the framebuffer. plat__rot: 0 none, 1 or 3 = the two directions.
// The choice comes from $PLAT_ROTATE, then the file /data/local/tmp/plat_rotate (a single digit), then it is automatic.
static int plat__rot, plat__lw, plat__lh;
static uint32_t *plat__lbuf;

static int plat__rotation_setting(void) {
    const char *e = getenv("PLAT_ROTATE");
    if (e && *e) return atoi(e);
    FILE *f = fopen("/data/local/tmp/plat_rotate", "r");
    if (f) { int v = -1; if (fscanf(f, "%d", &v) == 1) { fclose(f); return v; } fclose(f); }
    return -1;                                                  // not set: decide from the shape of the screen
}

PLAT_FN int plat_init(PlatInfo *pi) {
    plat__fd = open("/dev/graphics/fb0", O_RDWR);
    if (plat__fd < 0) plat__fd = open("/dev/fb0", O_RDWR);
    if (plat__fd < 0) { perror("open fb0"); return -1; }
    if (ioctl(plat__fd, FBIOGET_VSCREENINFO, &plat__v) || ioctl(plat__fd, FBIOGET_FSCREENINFO, &plat__f)) {
        perror("ioctl get"); return -1;
    }
    if (plat__v.bits_per_pixel != 32) { fprintf(stderr, "only 32bpp handled\n"); return -1; }
    if (plat__v.yres_virtual / plat__v.yres < 2) {                        // one page only: ask the driver for a second one
        struct fb_var_screeninfo want = plat__v; want.yres_virtual = plat__v.yres * 2;
        if (ioctl(plat__fd, FBIOPUT_VSCREENINFO, &want) == 0) {
            ioctl(plat__fd, FBIOGET_VSCREENINFO, &plat__v); ioctl(plat__fd, FBIOGET_FSCREENINFO, &plat__f);
            fprintf(stderr, "framebuffer: asked for 2 pages, now yres_virtual %u\n", plat__v.yres_virtual);
        }
    }
    plat__pages = plat__v.yres_virtual / plat__v.yres;
    if (plat__pages < 1) plat__pages = 1;
    plat__page = plat__pages > 1 ? 1 : 0;
    plat__base = mmap(NULL, plat__f.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, plat__fd, 0);
    if (plat__base == MAP_FAILED) { perror("mmap"); return -1; }
    if (!getenv("FB_PARENT")) plat__wake_panel();      // the launcher already woke the panel
    pi->name = "Android / fbdev (/dev/graphics/fb0)";
    pi->w = plat__v.xres; pi->h = plat__v.yres;
    int rs = plat__rotation_setting();
    plat__rot = rs >= 0 ? rs : (plat__v.xres > plat__v.yres ? 1 : 0);
    if (plat__rot != 0 && plat__rot != 1 && plat__rot != 3) plat__rot = 0;
    if (plat__rot) {
        plat__lw = plat__v.yres; plat__lh = plat__v.xres;
        plat__lbuf = calloc((size_t)plat__lw * plat__lh, sizeof(uint32_t));
        if (!plat__lbuf) plat__rot = 0;
        else { pi->w = plat__lw; pi->h = plat__lh; fprintf(stderr, "rotation %d: programs see %dx%d, framebuffer is %ux%u\n", plat__rot, plat__lw, plat__lh, plat__v.xres, plat__v.yres); }
    }
    pi->rs = plat__v.red.offset; pi->gs = plat__v.green.offset; pi->bs = plat__v.blue.offset;
    pi->alpha = 0;                          // most fbdev drivers ignore alpha
    // A display with an 8-bit alpha channel (the Kindle Fire's omapfb) can treat alpha 0 as transparent and show nothing.
    if (plat__v.transp.length == 8 && plat__v.transp.offset < 32) pi->alpha = 0xFFu << plat__v.transp.offset;
    // The LG Intuition reports red at bit 24, green 16, blue 8. Its display reads the bytes as R,G,B,A in memory order
    // (a photo of colour bars showed red lost, green and blue swapped), so use the byte order the display really uses.
    if (plat__v.red.offset == 24 && plat__v.green.offset == 16 && plat__v.blue.offset == 8 && plat__v.transp.offset == 0) {
        pi->rs = 0; pi->gs = 8; pi->bs = 16; pi->alpha = 0xFF000000u;
    }
    plat__last = plat__now();
    return 0;
}

PLAT_FN uint32_t *plat_begin(int *stride_px) {
    if (plat__rot) { *stride_px = plat__lw; return plat__lbuf; }
    *stride_px = plat__f.line_length / 4;
    return (uint32_t *)(plat__base + (size_t)plat__page * plat__v.yres * plat__f.line_length);
}

// Rotate the programs' buffer into the framebuffer page we are about to show.
static void plat__rotate_into_page(void) {
    uint32_t *dst0 = (uint32_t *)(plat__base + (size_t)plat__page * plat__v.yres * plat__f.line_length);
    int fw = plat__v.xres, fh = plat__v.yres, fs = plat__f.line_length / 4, lw = plat__lw, lh = plat__lh;
    for (int py = 0; py < fh; py++) {
        uint32_t *dst = dst0 + (size_t)py * fs;
        if (plat__rot == 1) { int lx = lw - 1 - py; for (int px = 0; px < fw; px++) dst[px] = plat__lbuf[(size_t)px * lw + lx]; }
        else                { int lx = py;          for (int px = 0; px < fw; px++) dst[px] = plat__lbuf[(size_t)(lh - 1 - px) * lw + lx]; }
    }
}

PLAT_FN void plat_flip(void) {
    if (plat__rot) plat__rotate_into_page();
    struct fb_var_screeninfo pv = plat__v;
    pv.yoffset = plat__page * plat__v.yres;
    ioctl(plat__fd, FBIOPAN_DISPLAY, &pv);
    uint32_t zero = 0;
    ioctl(plat__fd, FBIO_WAITFORVSYNC, &zero);              // fails on these devices; timer below caps fps
    plat__page = (plat__page + 1) % plat__pages;
    double t = plat__now();
    if (t - plat__last < 1.0 / 60) usleep((useconds_t)((1.0 / 60 - (t - plat__last)) * 1e6));
    plat__last = plat__now();
}

PLAT_FN void plat_close(void) {
    struct fb_var_screeninfo z = plat__v; z.yoffset = 0;
    ioctl(plat__fd, FBIOPAN_DISPLAY, &z);
    if (plat__bl_old >= 0) {                                 // put the backlight back
        int fd = open(plat__bl_path, O_WRONLY);
        if (fd >= 0) { char b[16]; int l = snprintf(b, sizeof b, "%d", plat__bl_old); (void)!write(fd, b, l); close(fd); }
    }
}

// ---- touch (evdev; handles multitouch protocol A and B, and single-touch) ----
#define PLAT_NSLOT 16
typedef struct { int active, x, y; } PlatSlot;
static PlatSlot plat__slots[PLAT_NSLOT], plat__pend[PLAT_NSLOT];
static int plat__tfd = -1, plat__cur, plat__protoA, plat__npend, plat__px, plat__py, plat__phas;
static int plat__mt, plat__minx, plat__maxx, plat__miny, plat__maxy, plat__sw, plat__sh;
static int plat__tportrait, plat__tflip;                          // panel axes are portrait while the screen is landscape; flip bits

static int plat__bit(const unsigned char *b, int n) { return (b[n / 8] >> (n % 8)) & 1; }

PLAT_FN int plat_touch_init(const PlatInfo *pi) {
    (void)pi;
    plat__sw = plat__v.xres; plat__sh = plat__v.yres;                   // touch is measured on the physical screen
    for (int i = 0; i < 32; i++) {
        char path[32]; snprintf(path, sizeof path, "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        unsigned char abs[ABS_MAX / 8 + 1], key[KEY_MAX / 8 + 1];
        memset(abs, 0, sizeof abs); memset(key, 0, sizeof key);
        ioctl(fd, EVIOCGBIT(EV_ABS, sizeof abs), abs);
        ioctl(fd, EVIOCGBIT(EV_KEY, sizeof key), key);
        int mt = plat__bit(abs, ABS_MT_POSITION_X) && plat__bit(abs, ABS_MT_POSITION_Y);
        int st = plat__bit(abs, ABS_X) && plat__bit(abs, ABS_Y) && plat__bit(key, BTN_TOUCH);
        if (!mt && !st) { close(fd); continue; }
        struct input_absinfo ax, ay;
        ioctl(fd, EVIOCGABS(mt ? ABS_MT_POSITION_X : ABS_X), &ax);
        ioctl(fd, EVIOCGABS(mt ? ABS_MT_POSITION_Y : ABS_Y), &ay);
        char name[64] = "?"; ioctl(fd, EVIOCGNAME(sizeof name), name);
        fprintf(stderr, "touch: %s (%s) %s x %d..%d y %d..%d\n", path, name, mt ? "multitouch" : "single",
                ax.minimum, ax.maximum, ay.minimum, ay.maximum);
        plat__tfd = fd; plat__mt = mt;
        plat__minx = ax.minimum; plat__maxx = ax.maximum;
        plat__miny = ay.minimum; plat__maxy = ay.maximum;
        plat__tportrait = (plat__maxx - plat__minx) < (plat__maxy - plat__miny) && plat__v.xres > plat__v.yres;
        { int v = 0; const char *e = getenv("PLAT_TOUCH_FLIP");
          if (e) v = atoi(e); else { FILE *f = fopen("/data/local/tmp/plat_touch", "r"); if (f) { if (fscanf(f, "%d", &v) != 1) v = 0; fclose(f); } }
          plat__tflip = v; }
        if (plat__tportrait) fprintf(stderr, "touch panel is portrait-native: flip bits %d\n", plat__tflip);
        return 0;
    }
    fprintf(stderr, "no touchscreen found in /dev/input\n");
    return -1;
}

PLAT_FN int plat_touch_poll(PlatTouch *out, int max) {
    struct input_event ev[64];
    for (;;) {
        ssize_t n = read(plat__tfd, ev, sizeof ev);
        if (n <= 0) break;                                   // EAGAIN: drained
        for (int i = 0; i < n / (int)sizeof(struct input_event); i++) {
            int t = ev[i].type, c = ev[i].code, v = ev[i].value;
            if (t == EV_ABS) {
                if (c == ABS_MT_SLOT) plat__cur = (v >= 0 && v < PLAT_NSLOT) ? v : 0;
                else if (c == ABS_MT_TRACKING_ID) plat__slots[plat__cur].active = v >= 0;
                else if (c == ABS_MT_POSITION_X) { plat__slots[plat__cur].x = plat__px = v; plat__phas |= 1; }
                else if (c == ABS_MT_POSITION_Y) { plat__slots[plat__cur].y = plat__py = v; plat__phas |= 2; }
                else if (!plat__mt && c == ABS_X) plat__slots[0].x = v;
                else if (!plat__mt && c == ABS_Y) plat__slots[0].y = v;
            } else if (t == EV_KEY && c == BTN_TOUCH && !plat__mt) {
                plat__slots[0].active = v;
            } else if (t == EV_SYN && c == 2 /* SYN_MT_REPORT */) {   // protocol A: one contact done
                plat__protoA = 1;
                if (plat__phas == 3 && plat__npend < PLAT_NSLOT) {
                    plat__pend[plat__npend].x = plat__px; plat__pend[plat__npend].y = plat__py;
                    plat__pend[plat__npend].active = 1; plat__npend++;
                }
                plat__phas = 0;
            } else if (t == EV_SYN && c == SYN_REPORT) {
                if (plat__protoA) {                                  // frame complete: contacts = pending list
                    for (int s = 0; s < PLAT_NSLOT; s++) {
                        plat__slots[s].active = s < plat__npend;
                        if (s < plat__npend) { plat__slots[s].x = plat__pend[s].x; plat__slots[s].y = plat__pend[s].y; }
                    }
                    plat__npend = 0;
                }
                plat__phas = 0;
            }
        }
    }
    int cnt = 0;
    for (int s = 0; s < PLAT_NSLOT && cnt < max; s++) {
        if (!plat__slots[s].active) continue;
        out[cnt].id = s;
        // Raw position as a fraction of the touch panel, 0..1 on each axis.
        double u = (plat__slots[s].x - plat__minx) / (double)(plat__maxx - plat__minx + 1);
        double v = (plat__slots[s].y - plat__miny) / (double)(plat__maxy - plat__miny + 1);
        if (plat__rot && plat__tportrait) {
            // The panel's own axes are portrait (the Kindle Fire: x 0..2305, y 0..3969) although the framebuffer is landscape.
            // Its axes then already follow the portrait picture the programs see. Flip bits come from /data/local/tmp/plat_touch.
            int lx = (int)(u * plat__lw), ly = (int)(v * plat__lh);
            if (plat__tflip & 1) lx = plat__lw - 1 - lx;
            if (plat__tflip & 2) ly = plat__lh - 1 - ly;
            if (plat__tflip & 4) { int t = lx * plat__lh / plat__lw; lx = ly * plat__lw / plat__lh; ly = t; }
            out[cnt].x = lx; out[cnt].y = ly;
        } else {
            int px = (int)(u * plat__sw), py = (int)(v * plat__sh);
            if (plat__rot == 1)      { out[cnt].x = plat__lw - 1 - py; out[cnt].y = px; }
            else if (plat__rot == 3) { out[cnt].x = py;               out[cnt].y = plat__lh - 1 - px; }
            else                     { out[cnt].x = px;               out[cnt].y = py; }
        }
        cnt++;
    }
    return cnt;
}

// For a launcher that waits on a child program. Nothing to refresh on Android.
PLAT_FN void plat_keepalive(void) {}
PLAT_FN void plat_emergency_cleanup(void) {}           // nothing to clean up: the display is the plain framebuffer
