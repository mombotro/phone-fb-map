// plat.h - tiny platform layer.
//   plat_init / plat_begin / plat_flip / plat_close          display (3-buffered)
//   plat_touch_init / plat_touch_poll                        multitouch
// plat_begin() returns the back buffer; plat_flip() shows it and rotates buffers.
#ifndef PLAT_H
#define PLAT_H
#include <stdint.h>
#include <stdio.h>

#define PLAT_FN static __attribute__((unused))

typedef struct {
    const char *name;
    int w, h;
    int rs, gs, bs;         // channel shifts of the pixel format
    uint32_t alpha;         // OR'd into every pixel
} PlatInfo;

typedef struct { int id, x, y; } PlatTouch;     // x,y in screen pixels

#ifdef __APPLE__
#include "plat_ios.h"
#else
#include "plat_android.h"
#endif

#endif
