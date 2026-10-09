// plat_ios.h - platform layer over ../fb.h (jailbroken iOS 7, iPhone 4). See plat.h.
// Touch: raw frames from the private MultitouchSupport.framework (AppleMultitouchSPIUserClient).
// The HID event system never delivers digitizer events to a second client on iOS 7, but this does.
// Freeze SpringBoard (not backboardd) while drawing, and kill -9 it afterwards for a clean respring
// so touches queued while it was stopped cannot hit icons.
#include "../fb.h"
#include <stdlib.h>

static FB plat__fb;
static int plat__w, plat__h;

PLAT_FN int plat_init(PlatInfo *pi) {
    if (fb_init(&plat__fb, 640, 960)) return -1;
    plat__fb.wait_mode = 3;                 // 60 Hz timer; SwapWait blocks with our guessed args
    pi->name = "iPhone 4 / iOS 7 (IOMobileFramebuffer)";
    pi->w = plat__w = 640; pi->h = plat__h = 960;
    pi->rs = 16; pi->gs = 8; pi->bs = 0;    // 0xAARRGGBB
    pi->alpha = 0xFF000000u;
    return 0;
}

PLAT_FN uint32_t *plat_begin(int *stride_px) {
    fb_begin(&plat__fb);
    *stride_px = (int)(plat__fb.stride / 4);
    return plat__fb.px;
}

PLAT_FN void plat_flip(void) { fb_flip(&plat__fb); }
PLAT_FN void plat_close(void) {}

// ---- touch: MultitouchSupport raw contact frames ----
// Layout matches the macOS MTFinger struct (23 words, double at offset 4): verified from dumped frames.
typedef struct {
    int frame; double timestamp; int identifier, state, fingerId, handId;
    float nx, ny, vx, vy;                      // normalized position (0..1) and velocity
    float size; int zero1; float angle, major, minor;
    float ax, ay, avx, avy; int zero2, zero3; float zdensity;
} PlatMTFinger;
_Static_assert(sizeof(PlatMTFinger) == 92, "MT finger layout");

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
    void (*Reg)(void *, void *) = dlsym(mt, "MTRegisterContactFrameCallback");
    int (*Start)(void *, int) = dlsym(mt, "MTDeviceStart");
    if (!Create || !Reg || !Start) { fprintf(stderr, "missing MT symbol\n"); return -1; }
    void *dev = Create();
    if (!dev) { fprintf(stderr, "MTDeviceCreateDefault failed\n"); return -1; }
    Reg(dev, (void *)plat__mt_cb);
    Start(dev, 0);
    fprintf(stderr, "touch: MultitouchSupport device %p, xform %d\n", dev, plat__mt_xform);
    return 0;
}

PLAT_FN int plat_touch_poll(PlatTouch *out, int max) {
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0, false);   // let the framework deliver pending frames
    int n = 0;
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
