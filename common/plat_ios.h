// plat_ios.h - platform layer over ../fb.h (jailbroken iOS 7, iPhone 4). See plat.h.
// Touch: raw frames from the private MultitouchSupport.framework (AppleMultitouchSPIUserClient).
// The HID event system never delivers digitizer events to a second client on iOS 7, but this does.
// Freeze SpringBoard (not backboardd) while drawing, and kill -9 it afterwards for a clean respring
// so touches queued while it was stopped cannot hit icons.
#ifdef __LP64__
#define FB_RECT_DOUBLE                       // iPhone 5s, iOS 12 (arm64): the display takes rectangles as doubles
#endif
#include "../fb.h"
#include <signal.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

static void plat__term_handler(int sig);
static FB plat__fb;
static int plat__w, plat__h;
static int plat__live;
static int plat__parent;                     // started by the launcher: it owns the flag and the overlay
static double plat__flag_t;
static int plat__screen_off;                 // /tmp/fbscreen_off exists: the power button turned the screen off
static double plat__off_check;
static double plat__reassert_until, plat__last_reassert;   // after the screen comes back on, keep re-showing our picture

PLAT_FN int plat_init(PlatInfo *pi) {
#ifdef __LP64__
    const int W0 = 640, H0 = 1136;
#else
    const int W0 = 640, H0 = 960;
#endif
    if (fb_init(&plat__fb, W0, H0)) return -1;
    plat__fb.wait_mode = 3;                 // 60 Hz timer; SwapWait blocks with our guessed args
    // Live SpringBoard mode: when the FBMode tweak is installed, SpringBoard keeps running (nothing for the system to
    // kill after two minutes). The program draws on overlay layer 1, which SpringBoard cannot cover, and creates
    // /tmp/fbmode. While that file is fresh, the tweak makes SpringBoard ignore touches and skip auto-lock.
    // Without the tweak the program uses layer 0 and the launch script freezes SpringBoard (limited to ~110 s).
    // PLAT_FROZEN=1 forces the old mode. PLAT_LAYER overrides the layer.
    plat__parent = getenv("FB_PARENT") != NULL;
    plat__live = access("/Library/MobileSubstrate/DynamicLibraries/FBMode.dylib", F_OK) == 0 && !getenv("PLAT_FROZEN");
    const char *lay = getenv("PLAT_LAYER");
#ifdef __LP64__
    plat__fb.layer = lay ? atoi(lay) : 1;       // iOS 12: layer 1 shows on top of the system picture
#else
    plat__fb.layer = lay ? atoi(lay) : (plat__live ? 1 : 0);
#endif
    if (plat__live && !plat__parent) {
        FILE *ff = fopen("/tmp/fbmode", "w");
        if (ff) fclose(ff);
        chmod("/tmp/fbmode", 0644);
    }
    signal(SIGTERM, plat__term_handler); signal(SIGINT, plat__term_handler); signal(SIGHUP, plat__term_handler);
    fprintf(stderr, "display: %s mode, layer %d\n", plat__live ? "live SpringBoard" : "frozen SpringBoard", plat__fb.layer);
#ifdef __LP64__
    pi->name = "iPhone 5s / iOS 12 (IOMobileFramebuffer)";
#else
    pi->name = "iPhone 4 / iOS 7 (IOMobileFramebuffer)";
#endif
    pi->w = plat__w = W0; pi->h = plat__h = H0;
    pi->rs = 16; pi->gs = 8; pi->bs = 0;    // 0xAARRGGBB
    pi->alpha = 0xFF000000u;
    return 0;
}

PLAT_FN uint32_t *plat_begin(int *stride_px) {
    fb_begin(&plat__fb);
    *stride_px = (int)(plat__fb.stride / 4);
    return plat__fb.px;
}

// Keep the flag fresh: the tweak ignores a flag older than 5 s. Programs that redraw only on change (music player,
// shell) may not flip for a long time, so the touch poll refreshes it too.
static void plat__flag_refresh(void) {
    if (!plat__live) return;
    struct timeval tv; gettimeofday(&tv, NULL);
    double t = tv.tv_sec + tv.tv_usec / 1e6;
    if (t - plat__flag_t >= 1.0) { utimes("/tmp/fbmode", NULL); plat__flag_t = t; }
    if (t - plat__off_check >= 0.2) {
        int off = access("/tmp/fbscreen_off", F_OK) == 0;
        if (plat__screen_off && !off) plat__reassert_until = t + 3.0;           // the screen just came back on
        plat__screen_off = off; plat__off_check = t;
    }
    if (t < plat__reassert_until && t - plat__last_reassert >= 0.1) { fb_reassert(&plat__fb); plat__last_reassert = t; }
}

// Flip statistics: a failed or slow flip may show on screen as flicker. Printed to stderr, at most one line per 5 s.
static long plat__flips, plat__fails, plat__slow;
static double plat__max_ms, plat__t0, plat__last_report;

PLAT_FN void plat_flip(void) {
    if (plat__screen_off) { usleep(60000); plat__flag_refresh(); return; }       // screen is off: do not draw
    struct timeval a, b; gettimeofday(&a, NULL);
    int rc = fb_flip(&plat__fb);
    gettimeofday(&b, NULL);
    double ms = (b.tv_sec - a.tv_sec) * 1000.0 + (b.tv_usec - a.tv_usec) / 1000.0;
    if (plat__t0 == 0) plat__t0 = plat__last_report = a.tv_sec + a.tv_usec / 1e6;
    plat__flips++;
    if (rc != 0) plat__fails++;
    if (ms > 40.0) plat__slow++;
    if (ms > plat__max_ms) plat__max_ms = ms;
    double t = b.tv_sec + b.tv_usec / 1e6;
    if (t - plat__last_report >= 5.0) {
        if (plat__fails || plat__slow)
            fprintf(stderr, "[%6.1f] flips: %ld, failed %ld, slow(>40ms) %ld, slowest %.0f ms\n", t - plat__t0, plat__flips, plat__fails, plat__slow, plat__max_ms);
        plat__last_report = t; plat__fails = plat__slow = 0; plat__max_ms = 0;
    }
    plat__flag_refresh();
}
PLAT_FN void plat_close(void) {
    if (plat__parent) return;                           // the launcher keeps the flag and the picture until it draws again
    if (plat__live) unlink("/tmp/fbmode");
    fb_unset(&plat__fb);
}

// For a launcher that waits on a child program: keep the flag fresh without drawing.
PLAT_FN void plat_keepalive(void) { plat__flag_refresh(); }

// Called when the program is killed or crashes: remove the flag and take our surface off the overlay layer,
// so no stale picture stays on screen. Best effort (it calls into IOKit from a signal handler).
PLAT_FN void plat_emergency_cleanup(void) {
    if (plat__parent) return;
    if (plat__live) unlink("/tmp/fbmode");
    if (plat__fb.disp) fb_unset(&plat__fb);
}

static void plat__term_handler(int sig) {
    plat_emergency_cleanup();
    _exit(128 + sig);
}

// ---- touch: MultitouchSupport raw contact frames ----
// Layout matches the macOS MTFinger struct (23 words, double at offset 4): verified from dumped frames.
typedef struct {
    int frame; double timestamp; int identifier, state, fingerId, handId;
    float nx, ny, vx, vy;                      // normalized position (0..1) and velocity
    float size; int zero1; float angle, major, minor;
    float ax, ay, avx, avy; int zero2, zero3; float zdensity;
} PlatMTFinger;
#ifndef __LP64__
_Static_assert(sizeof(PlatMTFinger) == 92, "MT finger layout");    // on arm64 the double is 8-aligned: 96 bytes, same fields
#endif

static struct { volatile int active; volatile float x, y; } plat__mt[16];
static int plat__mt_xform;                   // PLAT_MT_XFORM: bit0 flip x, bit1 flip y, bit2 swap x/y

static int plat__mt_cb(void *dev, PlatMTFinger *f, int n, double ts, int frame) {
    (void)dev; (void)ts; (void)frame;
    int seen[16] = {0};
    for (int i = 0; i < n; i++) {
        int id = f[i].fingerId & 15;
        if (f[i].state != 4 && f[i].state != 5) continue;           // 4/5 = touching
        seen[id] = 1;
        plat__mt[id].x = f[i].nx; plat__mt[id].y = f[i].ny;
        plat__mt[id].active = 1;
    }
    for (int i = 0; i < 16; i++) if (!seen[i]) plat__mt[i].active = 0;
    return 0;
}

PLAT_FN int plat_touch_init(const PlatInfo *pi) {
    (void)pi;
    // Transform precedence: $PLAT_MT_XFORM, then /var/root/mt_xform (written by calib), then 0.
    const char *x = getenv("PLAT_MT_XFORM");
    plat__mt_xform = x ? atoi(x) : 0;
    if (!x) {
        FILE *f = fopen("/var/root/mt_xform", "r");
        if (f) { int v; if (fscanf(f, "%d", &v) == 1) plat__mt_xform = v; fclose(f); }
    }
    void *mt = dlopen("/System/Library/PrivateFrameworks/MultitouchSupport.framework/MultitouchSupport", RTLD_NOW);
    if (!mt) { fprintf(stderr, "dlopen MultitouchSupport: %s\n", dlerror()); return -1; }
    void *(*Create)(void) = dlsym(mt, "MTDeviceCreateDefault");
    void *(*CreateList)(void) = dlsym(mt, "MTDeviceCreateList");
    CFRunLoopSourceRef (*MkSrc)(void *) = dlsym(mt, "MTDeviceCreateMultitouchRunLoopSource");
    void (*Reg)(void *, void *) = dlsym(mt, "MTRegisterContactFrameCallback");
    int (*Start)(void *, int) = dlsym(mt, "MTDeviceStart");
    if (!Create || !Reg || !Start) { fprintf(stderr, "missing MT symbol\n"); return -1; }
    void *dev = NULL;
#ifdef __LP64__
    // iOS 12: use the first device of the list, create its run-loop source before the callback and the start.
    if (CreateList) {
        CFArrayRef list = CreateList();
        if (list && CFArrayGetCount(list) > 0) dev = (void *)CFArrayGetValueAtIndex(list, 0);
    }
    if (dev && MkSrc) { CFRunLoopSourceRef rs = MkSrc(dev); if (rs) CFRunLoopAddSource(CFRunLoopGetCurrent(), rs, kCFRunLoopDefaultMode); }
#endif
    if (!dev) dev = Create();
    if (!dev) { fprintf(stderr, "MTDeviceCreateDefault failed\n"); return -1; }
    Reg(dev, (void *)plat__mt_cb);
    int rc = Start(dev, 0);
    if (rc) fprintf(stderr, "MTDeviceStart rc=0x%x\n", (unsigned)rc);
    fprintf(stderr, "touch: MultitouchSupport device %p, xform %d\n", dev, plat__mt_xform);
    return 0;
}

PLAT_FN int plat_touch_poll(PlatTouch *out, int max) {
    plat__flag_refresh();
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0, false);   // let the framework deliver pending frames
    int n = 0;
    if (plat__screen_off) return 0;                         // screen is off: ignore touches (no taps in a pocket)
    for (int i = 0; i < 16 && n < max; i++) {
        if (!plat__mt[i].active) continue;
        float x = plat__mt[i].x, y = plat__mt[i].y;
        if (plat__mt_xform & 4) { float t = x; x = y; y = t; }
        if (plat__mt_xform & 1) x = 1.0f - x;
        if (plat__mt_xform & 2) y = 1.0f - y;
        out[n].id = i;
        out[n].x = (int)(x * plat__w);
        out[n].y = (int)(y * plat__h);
        n++;
    }
    return n;
}

// Raw (untransformed) normalized position of the first touching finger; used by calib.
PLAT_FN int plat_mt_raw(float *x, float *y) {
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0, false);
    for (int i = 0; i < 16; i++)
        if (plat__mt[i].active) { *x = plat__mt[i].x; *y = plat__mt[i].y; return 1; }
    return 0;
}
