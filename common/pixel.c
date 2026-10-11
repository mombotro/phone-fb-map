// pixel.c - a pixel art editor for the iOS and Android framebuffers.
// usage: pixel [seconds] [dir]      (default 86400 seconds, dir DATA/pixel)
// Pictures are 16, 32, or 48 pixels square with a 16-color palette. Each picture is saved as NAME.pix (to edit again)
// and NAME.bmp (to open on a computer, 8 times larger). Tools: PEN, FILL, PICK (take a color from the picture). UNDO goes back 30 steps.
#include "plat.h"
#include "draw.h"
#include "kbd.h"
#include "ui.h"
#include "bmp.h"
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifdef __APPLE__
#define DEFAULT_DIR "/var/root/data/pixel"
#else
#define DEFAULT_DIR "/data/local/tmp/data/pixel"
#endif

static const uint32_t PAL[16] = { 0x000000, 0x1D2B53, 0x7E2553, 0x008751, 0xAB5236, 0x5F574F, 0xC2C3C7, 0xFFF1E8,
                                  0xFF004D, 0xFFA300, 0xFFEC27, 0x00E436, 0x29ADFF, 0x83769C, 0xFF77A8, 0xFFCCAA };
#define PAPER 7
#define MAXPIC 48
#define NUNDO 30
#define MAXFILES 200

typedef struct { char name[64]; int size; } Pic;
static Pic pics[MAXFILES]; static int npics;
static char dir[256] = DEFAULT_DIR;

static unsigned char px_[MAXPIC * MAXPIC]; static int psize = 32;
static unsigned char undo_[NUNDO][MAXPIC * MAXPIC]; static int nundo;

static int cmp_pic(const void *a, const void *b) { return strcmp(((const Pic *)b)->name, ((const Pic *)a)->name); }       // newest names first
static void scan_pics(void) {
    npics = 0;
    DIR *d = opendir(dir); if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && npics < MAXFILES) {
        size_t l = strlen(e->d_name);
        if (e->d_name[0] == '.' || l < 5 || strcmp(e->d_name + l - 4, ".pix") || l > 60) continue;
        snprintf(pics[npics].name, sizeof pics[0].name, "%.*s", (int)(l - 4), e->d_name);
        char p[400]; snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        FILE *f = fopen(p, "rb"); unsigned char h[6];
        pics[npics].size = (f && fread(h, 1, 6, f) == 6 && !memcmp(h, "PIX1", 4)) ? h[4] : 0;
        if (f) fclose(f);
        if (pics[npics].size) npics++;
    }
    closedir(d);
    qsort(pics, (size_t)npics, sizeof pics[0], cmp_pic);
}
static int load_pic(const char *name) {
    char p[400]; snprintf(p, sizeof p, "%s/%s.pix", dir, name);
    FILE *f = fopen(p, "rb"); if (!f) return -1;
    unsigned char h[6];
    if (fread(h, 1, 6, f) != 6 || memcmp(h, "PIX1", 4) || h[4] < 1 || h[4] > MAXPIC || h[4] != h[5]) { fclose(f); return -1; }
    psize = h[4]; int ok = fread(px_, 1, (size_t)psize * psize, f) == (size_t)psize * psize; fclose(f);
    for (int i = 0; i < psize * psize; i++) px_[i] &= 15;
    return ok ? 0 : -1;
}
static int save_pic(const char *name) {
    mkdir(dir, 0755);
    char p[400]; snprintf(p, sizeof p, "%s/%s.pix", dir, name);
    FILE *f = fopen(p, "wb"); if (!f) return -1;
    unsigned char h[6] = { 'P', 'I', 'X', '1', (unsigned char)psize, (unsigned char)psize };
    fwrite(h, 1, 6, f); fwrite(px_, 1, (size_t)psize * psize, f);
    if (fclose(f)) return -1;
    uint32_t rgb[MAXPIC * MAXPIC];
    for (int i = 0; i < psize * psize; i++) rgb[i] = PAL[px_[i] & 15];
    snprintf(p, sizeof p, "%s/%s.bmp", dir, name);
    return bmp_save(p, rgb, psize, psize, 8);
}
static void push_undo(void) {
    if (nundo == NUNDO) { memmove(undo_[0], undo_[1], sizeof undo_[0] * (NUNDO - 1)); nundo--; }
    memcpy(undo_[nundo++], px_, sizeof px_);
}
static void flood(int x, int y, int to) {
    int from = px_[y * psize + x]; if (from == to) return;
    int *stack = malloc(sizeof(int) * psize * psize * 4 + 16); if (!stack) return;
    int sp = 0; stack[sp++] = y * psize + x;
    while (sp > 0) {
        int i = stack[--sp], cx = i % psize, cy = i / psize;
        if (px_[i] != from) continue;
        px_[i] = (unsigned char)to;
        if (cx > 0) stack[sp++] = i - 1;
        if (cx < psize - 1) stack[sp++] = i + 1;
        if (cy > 0) stack[sp++] = i - psize;
        if (cy < psize - 1) stack[sp++] = i + psize;
        if (sp > psize * psize * 4 - 8) break;
    }
    free(stack);
}

enum { V_LIST, V_EDIT };
enum { T_PEN, T_FILL, T_PICK };

int main(int argc, char **argv) {
    double seconds = argc > 1 ? atof(argv[1]) : 86400;
    if (argc > 2) snprintf(dir, sizeof dir, "%s", argv[2]);
    PlatInfo pi;
    if (plat_init(&pi)) return 1;
    if (plat_touch_init(&pi)) return 1;
    int W = pi.w, H = pi.h;
    Ui u; memset(&u, 0, sizeof u);
    UiScroll sc = {0};
    int view = V_LIST, quit = 0, dirty = 3, tool = T_PEN, color = 0, confirm_del = 0;
    int prev_down = 0, last_cx = -1, last_cy = -1;
    char name[64] = ""; char msg[64] = ""; double msg_t = 0;
    double t0 = ui_now();
    const int BAR = 64, ROW = 84;
    mkdir(dir, 0755);
    scan_pics();

    while (!quit && ui_now() - t0 < seconds) {
        PlatTouch tc[10];
        int n = plat_touch_poll(tc, 10);
        ui_poll(&u, tc, n);
        if (u.changed || u.tapped || u.down) dirty = 3;
        if (msg[0] && ui_now() - msg_t > 2.5) { msg[0] = 0; dirty = 3; }
        if (dirty <= 0) { usleep(6000); continue; }
        dirty--;

        int stride; uint32_t *fb = plat_begin(&stride);
        Surf s = { fb, stride, W, H, pi.rs, pi.gs, pi.bs, pi.alpha, 0 };
        u.s = &s; u.W = W; u.H = H;
        draw_rect(&s, 0, 0, W, H, 0x10161E);
        draw_rect(&s, 0, 0, W, BAR, 0x26303E);

        if (view == V_LIST) {
            ui_label(&u, 12, 20, 2, "PIXEL", 0xFFFFFF);
            int bw = (W - 140 - 136 - 6 * 4) / 3;
            static const int SZ[3] = { 16, 32, 48 };
            for (int k = 0; k < 3; k++) {
                char lb[16]; snprintf(lb, sizeof lb, "+%d", SZ[k]);
                if (ui_button(&u, 110 + k * (bw + 6), 8, bw, BAR - 16, lb, 0x2E7D4F)) {
                    psize = SZ[k]; memset(px_, PAPER, sizeof px_); nundo = 0; color = 0; tool = T_PEN;
                    snprintf(name, sizeof name, "art%ld", (long)time(NULL)); view = V_EDIT; confirm_del = 0; dirty = 3;
                }
            }
            if (ui_button(&u, W - 128, 8, 120, BAR - 16, "EXIT", 0x883333)) quit = 1;
            int ly = BAR, lh = H - BAR;
            ui_scroll(&u, &sc, 0, ly, W, lh, npics * ROW);
            for (int i = 0; i < npics; i++) {
                int y = ly + i * ROW - sc.pos;
                if (y + ROW <= ly || y >= ly + lh) continue;
                char buf[96];
                draw_rect(&s, 0, y, W, ROW - 2, 0x18202A);
                ui_fit(buf, sizeof buf, pics[i].name, (W - 40) / 16);
                ui_label(&u, 16, y + 12, 2, buf, 0xF0F0F0);
                snprintf(buf, sizeof buf, "%d x %d", pics[i].size, pics[i].size);
                ui_label(&u, 16, y + 54, 1, buf, 0x8899AA);
                if (u.tapped && u.tap_y >= ly && ui_inside(u.tap_x, u.tap_y, 0, y, W, ROW)) {
                    if (load_pic(pics[i].name) == 0) { snprintf(name, sizeof name, "%s", pics[i].name); nundo = 0; color = 0; tool = T_PEN; view = V_EDIT; confirm_del = 0; }
                    u.tapped = 0; dirty = 3;
                }
            }
            if (!npics) ui_label(&u, 16, ly + 24, 2, "No pictures yet. Tap +32.", 0x8899AA);
        } else {
            // layout: bar, square canvas, palette (two rows of 8)
            int palh = 2 * ((W - 16) / 8 < 70 ? (W - 16) / 8 : 70) + 12;
            int avail_h = H - BAR - palh - 8, avail_w = W - 16;
            int cell = (avail_w < avail_h ? avail_w : avail_h) / psize; if (cell < 2) cell = 2;
            int cw = cell * psize, cx0 = (W - cw) / 2, cy0 = BAR + 4 + (avail_h - cw) / 2;
            // drawing with the finger
            if (u.down && ui_inside(u.start_x, u.start_y, cx0, cy0, cw, cw)) {
                int gx = (u.x - cx0) / cell, gy = (u.y - cy0) / cell;
                if (gx < 0) gx = 0; if (gx >= psize) gx = psize - 1; if (gy < 0) gy = 0; if (gy >= psize) gy = psize - 1;
                if (tool == T_PEN) {
                    if (!prev_down) { push_undo(); last_cx = gx; last_cy = gy; }
                    int x0 = last_cx, y0 = last_cy, dx = abs(gx - x0), dy = abs(gy - y0), sx = x0 < gx ? 1 : -1, sy = y0 < gy ? 1 : -1, err = dx - dy;
                    for (;;) {                                           // every cell between the last point and this one
                        px_[y0 * psize + x0] = (unsigned char)color;
                        if (x0 == gx && y0 == gy) break;
                        int e2 = 2 * err; if (e2 > -dy) { err -= dy; x0 += sx; } if (e2 < dx) { err += dx; y0 += sy; }
                    }
                    last_cx = gx; last_cy = gy; dirty = 3;
                }
            }
            if (u.tapped && ui_inside(u.tap_x, u.tap_y, cx0, cy0, cw, cw)) {
                int gx = (u.tap_x - cx0) / cell, gy = (u.tap_y - cy0) / cell;
                if (gx >= 0 && gx < psize && gy >= 0 && gy < psize) {
                    if (tool == T_FILL) { push_undo(); flood(gx, gy, color); }
                    else if (tool == T_PICK) { color = px_[gy * psize + gx]; tool = T_PEN; }
                    else if (px_[gy * psize + gx] != color) { push_undo(); px_[gy * psize + gx] = (unsigned char)color; }      // a quick tap with the pen
                    dirty = 3;
                }
            }
            prev_down = u.down && ui_inside(u.start_x, u.start_y, cx0, cy0, cw, cw);
            // the picture
            draw_rect(&s, cx0 - 2, cy0 - 2, cw + 4, cw + 4, 0x4A5568);
            for (int gy = 0; gy < psize; gy++) for (int gx = 0; gx < psize; gx++)
                draw_rect(&s, cx0 + gx * cell, cy0 + gy * cell, cell, cell, PAL[px_[gy * psize + gx] & 15]);
            if (cell >= 8) {                                             // a faint grid when the cells are big enough to see
                for (int g = 1; g < psize; g++) { draw_rect(&s, cx0 + g * cell, cy0, 1, cw, 0x2A3442); draw_rect(&s, cx0, cy0 + g * cell, cw, 1, 0x2A3442); }
            }
            // top bar
            int bw = (W - 7 * 5) / 6;
            if (ui_button(&u, 5, 8, bw, BAR - 16, "BACK", 0x883333)) {
                if (save_pic(name) != 0) { snprintf(msg, sizeof msg, "Save failed"); msg_t = ui_now(); }
                scan_pics(); view = V_LIST; dirty = 3;
            }
            if (ui_button(&u, 10 + bw, 8, bw, BAR - 16, "UNDO", 0x34608F) && nundo > 0) { memcpy(px_, undo_[--nundo], sizeof px_); dirty = 3; }
            if (ui_button(&u, 15 + 2 * bw, 8, bw, BAR - 16, tool == T_PEN ? "PEN*" : "PEN", tool == T_PEN ? 0xB06020 : 0x34608F)) tool = T_PEN;
            if (ui_button(&u, 20 + 3 * bw, 8, bw, BAR - 16, tool == T_FILL ? "FILL*" : "FILL", tool == T_FILL ? 0xB06020 : 0x34608F)) tool = T_FILL;
            if (ui_button(&u, 25 + 4 * bw, 8, bw, BAR - 16, tool == T_PICK ? "PICK*" : "PICK", tool == T_PICK ? 0xB06020 : 0x34608F)) tool = T_PICK;
            if (ui_button(&u, 30 + 5 * bw, 8, bw, BAR - 16, confirm_del ? "SURE?" : "DEL", 0x883333)) {
                if (confirm_del) {
                    char p[400]; snprintf(p, sizeof p, "%s/%s.pix", dir, name); unlink(p);
                    snprintf(p, sizeof p, "%s/%s.bmp", dir, name); unlink(p);
                    scan_pics(); view = V_LIST; confirm_del = 0;
                } else confirm_del = 1;
                dirty = 3;
            } else if (u.tapped && !ui_inside(u.tap_x, u.tap_y, 30 + 5 * bw, 8, bw, BAR - 16)) confirm_del = 0;
            // palette
            int sw = (W - 16) / 8; if (sw > 70) sw = 70;
            int px0 = (W - 8 * sw) / 2, py0 = H - palh + 6;
            for (int i = 0; i < 16; i++) {
                int x = px0 + (i % 8) * sw, y = py0 + (i / 8) * sw;
                draw_rect(&s, x + 2, y + 2, sw - 4, sw - 4, color == i ? 0xFFFFFF : 0x26303E);
                draw_rect(&s, x + 6, y + 6, sw - 12, sw - 12, PAL[i]);
                if (u.tapped && ui_inside(u.tap_x, u.tap_y, x, y, sw, sw)) { color = i; if (tool == T_PICK) tool = T_PEN; dirty = 3; }
            }
            if (msg[0]) { draw_rect(&s, 0, H - palh - 30, W, 30, 0x883333); draw_text(&s, 12, H - palh - 22, 1, msg, 0xFFFFFF); }
        }
        plat_flip();
    }
    if (view == V_EDIT) save_pic(name);
    plat_close();
    return 0;
}
