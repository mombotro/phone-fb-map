// bmp.h - write a 24-bit BMP file, which any computer opens. Plain C.
#ifndef BMP_H
#define BMP_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// rgb: w*h pixels as 0xRRGGBB, top row first. scale repeats each pixel scale x scale times. Returns 0 on success.
static int bmp_save(const char *path, const uint32_t *rgb, int w, int h, int scale) {
    if (w <= 0 || h <= 0 || scale < 1) return -1;
    int ow = w * scale, oh = h * scale, row = (ow * 3 + 3) & ~3;
    FILE *f = fopen(path, "wb"); if (!f) return -1;
    uint32_t size = 54 + (uint32_t)row * (uint32_t)oh;
    unsigned char hd[54] = { 'B', 'M' };
    #define BMP_W32(o, v) do { hd[o] = (unsigned char)(v); hd[o+1] = (unsigned char)((v) >> 8); hd[o+2] = (unsigned char)((v) >> 16); hd[o+3] = (unsigned char)((v) >> 24); } while (0)
    BMP_W32(2, size); BMP_W32(10, 54u); BMP_W32(14, 40u); BMP_W32(18, (uint32_t)ow); BMP_W32(22, (uint32_t)oh);
    hd[26] = 1; hd[28] = 24; BMP_W32(34, (uint32_t)row * (uint32_t)oh);
    #undef BMP_W32
    fwrite(hd, 1, 54, f);
    unsigned char *line = calloc(1, (size_t)row); if (!line) { fclose(f); return -1; }
    for (int y = oh - 1; y >= 0; y--) {                              // BMP rows run bottom to top
        const uint32_t *src = rgb + (size_t)(y / scale) * w;
        for (int x = 0; x < ow; x++) { uint32_t c = src[x / scale]; line[x * 3] = (unsigned char)c; line[x * 3 + 1] = (unsigned char)(c >> 8); line[x * 3 + 2] = (unsigned char)(c >> 16); }
        fwrite(line, 1, (size_t)row, f);
    }
    free(line);
    return fclose(f) == 0 ? 0 : -1;
}
#endif
