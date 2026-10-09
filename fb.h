// fb.h - double-buffered framebuffer for jailbroken iOS 7 (iPhone 4, 640x960 BGRA)
// Draws into two private IOSurfaces and submits them via IOMobileFramebufferSwap*.
// Freeze the compositor first: killall -STOP backboardd
#ifndef FB_H
#define FB_H

#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/time.h>
#include <unistd.h>

typedef void *FB_IOMFB;
typedef void *FB_Surface;
typedef struct { float x, y, w, h; } FB_Rect;   // same register/stack layout as 4 ints on armv7

typedef struct {
    FB_IOMFB disp;
    FB_Surface surf[3];
    int back;                       // index of surface being drawn to
    size_t w, h, stride;            // stride in bytes
    uint32_t *px;                   // back buffer pixels (valid between fb_begin and fb_flip)

    int (*SwapBegin)(FB_IOMFB, uint32_t *);
    int (*SwapSetLayer)(FB_IOMFB, int, FB_Surface, FB_Rect, FB_Rect, uint32_t);
    int (*SwapEnd)(FB_IOMFB);
    int (*Lock)(FB_Surface, uint32_t, uint32_t *);
    int (*Unlock)(FB_Surface, uint32_t, uint32_t *);
    void *(*Base)(FB_Surface);
    int (*SwapWait)(FB_IOMFB, uint32_t, uint32_t);
    int wait_mode;                  // 0 = none, 1/2 = SwapWait (BLOCKS - args wrong), 3 = 60Hz timer
    uint32_t last_token;
} FB;

static CFNumberRef fb__num(int v) { return CFNumberCreate(NULL, kCFNumberSInt32Type, &v); }

// Returns 0 on success.
static int fb_init(FB *f, int w, int h) {
    void *fw = dlopen("/System/Library/PrivateFrameworks/IOMobileFramebuffer.framework/IOMobileFramebuffer", RTLD_NOW);
    void *is = dlopen("/System/Library/PrivateFrameworks/IOSurface.framework/IOSurface", RTLD_NOW);
    if (!fw || !is) { fprintf(stderr, "dlopen: %s\n", dlerror()); return -1; }

    int (*GetMainDisplay)(FB_IOMFB *) = dlsym(fw, "IOMobileFramebufferGetMainDisplay");
    FB_Surface (*Create)(CFDictionaryRef) = dlsym(is, "IOSurfaceCreate");
    f->SwapBegin = dlsym(fw, "IOMobileFramebufferSwapBegin");
    f->SwapSetLayer = dlsym(fw, "IOMobileFramebufferSwapSetLayer");
    f->SwapEnd = dlsym(fw, "IOMobileFramebufferSwapEnd");
    f->Lock = dlsym(is, "IOSurfaceLock");
    f->Unlock = dlsym(is, "IOSurfaceUnlock");
    f->Base = dlsym(is, "IOSurfaceGetBaseAddress");
    f->SwapWait = dlsym(fw, "IOMobileFramebufferSwapWait");
    f->wait_mode = 0; f->last_token = 0;
    if (!GetMainDisplay || !Create || !f->SwapBegin || !f->SwapSetLayer || !f->SwapEnd ||
        !f->Lock || !f->Unlock || !f->Base) {
        fprintf(stderr, "missing symbol\n"); return -1;
    }
    if (GetMainDisplay(&f->disp) != 0 || !f->disp) { fprintf(stderr, "no main display\n"); return -1; }

    f->w = (size_t)w; f->h = (size_t)h; f->stride = (size_t)w * 4;
    for (int i = 0; i < 3; i++) {
        CFMutableDictionaryRef d = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFDictionarySetValue(d, CFSTR("IOSurfaceWidth"), fb__num(w));
        CFDictionarySetValue(d, CFSTR("IOSurfaceHeight"), fb__num(h));
        CFDictionarySetValue(d, CFSTR("IOSurfaceBytesPerElement"), fb__num(4));
        CFDictionarySetValue(d, CFSTR("IOSurfaceBytesPerRow"), fb__num(w * 4));
        CFDictionarySetValue(d, CFSTR("IOSurfacePixelFormat"), fb__num(0x42475241));  // 'BGRA'
        CFDictionarySetValue(d, CFSTR("IOSurfaceAllocSize"), fb__num(w * 4 * h));
        CFDictionarySetValue(d, CFSTR("IOSurfaceIsGlobal"), kCFBooleanTrue);
        CFDictionarySetValue(d, CFSTR("IOSurfaceMemoryRegion"), CFSTR("PurpleGFXMem"));
        f->surf[i] = Create(d);
        if (!f->surf[i]) { fprintf(stderr, "IOSurfaceCreate %d failed\n", i); return -1; }
    }
    f->back = 0;
    f->px = NULL;
    return 0;
}

// Lock the back buffer; f->px is then valid. Pixels are 0xAARRGGBB when read as uint32_t (BGRA bytes).
static void fb_begin(FB *f) {
    f->Lock(f->surf[f->back], 0, NULL);
    f->px = (uint32_t *)f->Base(f->surf[f->back]);
}

// Unlock the back buffer, put it on screen, and make the other surface the new back buffer.
static int fb_flip(FB *f) {
    f->Unlock(f->surf[f->back], 0, NULL);
    f->px = NULL;
    uint32_t token = 0;
    FB_Rect r = {0, 0, (float)f->w, (float)f->h};
    int rc = f->SwapBegin(f->disp, &token);
    if (rc == 0) rc = f->SwapSetLayer(f->disp, 0, f->surf[f->back], r, r, 0);
    if (rc == 0) rc = f->SwapEnd(f->disp);
    if (rc == 0 && f->SwapWait && f->wait_mode == 1) rc = f->SwapWait(f->disp, token, 1000);
    if (rc == 0 && f->SwapWait && f->wait_mode == 2 && f->last_token) rc = f->SwapWait(f->disp, f->last_token, 1000);
    if (f->wait_mode == 3) {        // timer pacing: hold each frame ~16.7ms
        static struct timeval last;
        struct timeval nowt; gettimeofday(&nowt, NULL);
        long el = (nowt.tv_sec - last.tv_sec) * 1000000L + (nowt.tv_usec - last.tv_usec);
        if (el >= 0 && el < 16667) usleep(16667 - el);
        gettimeofday(&last, NULL);
    }
    f->last_token = token;
    f->back = (f->back + 1) % 3;
    return rc;
}

#endif
