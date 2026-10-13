// ttf.h - fonts from TrueType and OpenType files, for every program and for the Reader and the Bible. Needs
// common/third_party/stb_truetype.h (common/fetch_stb_truetype.sh). Included by draw.h when UI_FONT is defined (plat.h does that).
// A font file lives in DATA/fonts, and the file DATA/fonts/choice holds its name (empty or missing: the chunky bitmap font).
//   draw_text()              (draw.h) uses the chosen font everywhere, sized so an average letter is as wide as a chunky cell.
//                            Editors and the terminal keep the chunky font, because they place the caret by character cell.
//   rdr_use(dir)             the Reader and the Bible: load the font for book text (a book is wrapped by true letter widths)
//   rdr_size, rdr_wrap, rdr_width, rdr_lineh, rdr_draw   book text with that font
//   ui_font_reload()         read the choice again (the picker calls it after the choice changes)
#ifndef TTF_H
#define TTF_H
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "third_party/stb_truetype.h"
#include "draw.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __APPLE__
#define TTF_FONT_DIR "/var/root/data/fonts"
#else
#define TTF_FONT_DIR "/data/local/tmp/data/fonts"
#endif

typedef struct { unsigned char *bm; int w, h, xo, yo, adv16, done; } TtfGlyph;       // adv16: advance in 1/16 pixel
typedef struct {
    unsigned char *data; stbtt_fontinfo fi; int ok, px; float scale; int ascent, descent;
    TtfGlyph g[95];
} TtfFont;

static void ttf_free_glyphs(TtfFont *f) { for (int i = 0; i < 95; i++) { if (f->g[i].bm) stbtt_FreeBitmap(f->g[i].bm, NULL); memset(&f->g[i], 0, sizeof f->g[i]); } }
static void ttf_close(TtfFont *f) { ttf_free_glyphs(f); free(f->data); memset(f, 0, sizeof *f); }
static int ttf_open(TtfFont *f, const char *path) {
    memset(f, 0, sizeof *f);
    FILE *fp = fopen(path, "rb"); if (!fp) return -1;
    fseek(fp, 0, SEEK_END); long n = ftell(fp); fseek(fp, 0, SEEK_SET);
    if (n < 1000 || n > (16L << 20)) { fclose(fp); return -1; }
    f->data = malloc((size_t)n); if (!f->data) { fclose(fp); return -1; }
    if (fread(f->data, 1, (size_t)n, fp) != (size_t)n) { fclose(fp); free(f->data); f->data = NULL; return -1; }
    fclose(fp);
    if (!stbtt_InitFont(&f->fi, f->data, stbtt_GetFontOffsetForIndex(f->data, 0))) { free(f->data); f->data = NULL; return -1; }
    f->ok = 1; return 0;
}
static void ttf_set_px(TtfFont *f, int px) {
    if (!f->ok || f->px == px) return;
    ttf_free_glyphs(f); f->px = px; f->scale = stbtt_ScaleForMappingEmToPixels(&f->fi, (float)px);       // px is the em square, so every font looks about as big
    int a, d, lg; stbtt_GetFontVMetrics(&f->fi, &a, &d, &lg);
    f->ascent = (int)(a * f->scale + 0.5f); f->descent = (int)(d * f->scale - 0.5f);
}
static TtfGlyph *ttf_glyph(TtfFont *f, int c) {
    if (c < 32 || c > 126) c = '?';
    TtfGlyph *g = &f->g[c - 32];
    if (!g->done) {
        int adv, lsb; stbtt_GetCodepointHMetrics(&f->fi, c, &adv, &lsb);
        g->adv16 = (int)(adv * f->scale * 16 + 0.5f);
        g->bm = stbtt_GetCodepointBitmap(&f->fi, 0, f->scale, c, &g->w, &g->h, &g->xo, &g->yo);
        g->done = 1;
    }
    return g;
}
// Draw len characters; base is the y of the baseline. Alpha-blends the glyphs. The colour keeps its contrast in the palmtop look.
static void ttf_draw_run(Surf *s, TtfFont *f, int x, int base, int y_probe, const char *t, int len, uint32_t rgb) {
    uint32_t want = rgb;
    if (s->theme) {                                                          // the same contrast rule as draw_text_plain
        want = draw_theme_color(s->theme, rgb);
        if (x >= 0 && y_probe >= 0 && x < s->w && y_probe < s->h) {
            uint32_t d = s->px[(size_t)y_probe * s->stride + x];
            uint32_t dest = (((d >> s->rs) & 255) << 16) | (((d >> s->gs) & 255) << 8) | ((d >> s->bs) & 255);
            want = draw_theme_text(s->theme, dest, rgb);
        }
    }
    int fr = (want >> 16) & 255, fg = (want >> 8) & 255, fb = want & 255;
    long pen = (long)x * 16;
    for (int i = 0; i < len; i++) {
        TtfGlyph *g = ttf_glyph(f, (unsigned char)t[i]);
        int gx = (int)((pen + 8) / 16) + g->xo, gy = base + g->yo;
        for (int yy = 0; yy < g->h; yy++) {
            int py = gy + yy; if (py < 0 || py >= s->h) continue;
            for (int xx = 0; xx < g->w; xx++) {
                int a = g->bm[yy * g->w + xx], px = gx + xx; if (!a || px < 0 || px >= s->w) continue;
                uint32_t *dp = &s->px[(size_t)py * s->stride + px]; uint32_t d = *dp;
                int dr = (d >> s->rs) & 255, dg = (d >> s->gs) & 255, db = (d >> s->bs) & 255;
                int r = (fr * a + dr * (255 - a)) / 255, g2 = (fg * a + dg * (255 - a)) / 255, b = (fb * a + db * (255 - a)) / 255;
                *dp = ((uint32_t)r << s->rs) | ((uint32_t)g2 << s->gs) | ((uint32_t)b << s->bs) | s->alpha;
            }
        }
        pen += g->adv16;
    }
}
static int ttf_choice(const char *dir, char *path, size_t n) {       // the font file named in DIR/choice
    char p[300], name[200] = "";
    snprintf(p, sizeof p, "%s/choice", dir);
    FILE *f = fopen(p, "r");
    if (f) { if (fgets(name, sizeof name, f)) name[strcspn(name, "\r\n")] = 0; fclose(f); }
    if (!name[0] || strchr(name, '/')) return 0;
    snprintf(path, n, "%s/%s", dir, name);
    return 1;
}

// ---- the font for every draw_text (menus, buttons, lists)
static TtfFont g_uif[7]; static int g_uif_state, g_uif_avg100, g_uif_pct = 90; static char g_uif_path[320];     // state: 0 not read yet, 1 font chosen, -1 chunky
// The size of the menu font in percent (60 to 130), in DATA/fonts/scale. The default 90 suits fonts that are wider than the chunky one.
static void ui_font_set_pct(int pct) {
    g_uif_pct = pct < 60 ? 60 : pct > 130 ? 130 : pct;
    for (int i = 0; i < 7; i++) if (g_uif[i].data) ttf_close(&g_uif[i]);                      // the sizes reopen when they are next used
}
static void ui_font_save_pct(void) { FILE *f = fopen(TTF_FONT_DIR "/scale", "w"); if (f) { fprintf(f, "%d\n", g_uif_pct); fclose(f); } }
static void ui_font_reload(void) {
    { FILE *f = fopen(TTF_FONT_DIR "/scale", "r"); int v; if (f) { if (fscanf(f, "%d", &v) == 1 && v >= 60 && v <= 130) g_uif_pct = v; fclose(f); } }
    for (int i = 0; i < 7; i++) if (g_uif[i].data) ttf_close(&g_uif[i]);
    g_uif_state = -1;
    if (!ttf_choice(TTF_FONT_DIR, g_uif_path, sizeof g_uif_path)) return;
    TtfFont probe; if (ttf_open(&probe, g_uif_path) != 0) return;
    ttf_set_px(&probe, 100);                                                 // the average letter width of English text, to match the chunky cell
    static const char *SAMPLE = "the quick brown fox jumps over the lazy dog and some more words ";
    long w = 0, n = (long)strlen(SAMPLE); for (long i = 0; i < n; i++) w += ttf_glyph(&probe, (unsigned char)SAMPLE[i])->adv16;
    g_uif_avg100 = (int)(w / n / 16); ttf_close(&probe);
    if (g_uif_avg100 > 5) g_uif_state = 1;
}
static int ui_font_draw(Surf *s, int x, int y, int scale, const char *str, uint32_t rgb) {
    if (g_uif_state == 0) ui_font_reload();
    if (g_uif_state < 0 || scale < 1 || scale > 6) return 0;
    TtfFont *f = &g_uif[scale];
    if (!f->ok) {
        if (ttf_open(f, g_uif_path) != 0) { g_uif_state = -1; return 0; }
        int px = 8 * scale * g_uif_pct / g_uif_avg100; if (px < 6) px = 6;               // pct/100 of the size that matches a chunky cell
        ttf_set_px(f, px);
    }
    int cell = 16 * scale, base = y + (cell - (f->ascent - f->descent)) / 2 + f->ascent;
    ttf_draw_run(s, f, x, base, y, str, (int)strlen(str), rgb);
    return 1;
}

// ---- the font for book text (Reader, Bible): true letter widths, so text is wrapped by pixels
static TtfFont g_ttf; static int g_ttf_on, g_rdr_fs = 2;
static const int RDR_PX[4] = { 0, 22, 30, 42 };
static int rdr_use(const char *dir) {
    char p[320]; g_ttf_on = 0;
    if (!ttf_choice(dir, p, sizeof p)) return 0;
    if (ttf_open(&g_ttf, p) == 0) { g_ttf_on = 1; ttf_set_px(&g_ttf, RDR_PX[g_rdr_fs]); }
    return g_ttf_on;
}
static void rdr_size(int fs) { g_rdr_fs = fs < 1 ? 1 : fs > 3 ? 3 : fs; if (g_ttf_on) ttf_set_px(&g_ttf, RDR_PX[g_rdr_fs]); }
static int rdr_lineh(void) {
    if (!g_ttf_on) return 16 * g_rdr_fs + 4;
    int h = (g_ttf.ascent - g_ttf.descent) * 12 / 10 + 2, cap = g_ttf.px * 17 / 10;      // some fonts have a very tall line box: keep it in range
    return h > cap ? cap : h;
}
static int rdr_char16(int c) { return g_ttf_on ? ttf_glyph(&g_ttf, c)->adv16 : 8 * g_rdr_fs * 16; }       // character width in 1/16 pixel
static int rdr_width(const char *t, int len) { long w = 0; for (int i = 0; i < len; i++) w += rdr_char16((unsigned char)t[i]); return (int)(w / 16); }
static int rdr_wrap(const char *t, int len, int maxw) {
    long w = 0, lim = (long)maxw * 16; int brk = -1, i;
    for (i = 0; i < len; i++) {
        if (t[i] == ' ') brk = i;
        w += rdr_char16((unsigned char)t[i]);
        if (w > lim) { if (i == 0) return 1; return brk > 0 ? brk : i; }
    }
    return len;
}
static void rdr_draw(Surf *s, int x, int y, const char *t, int len, uint32_t rgb) {
    if (!g_ttf_on) { char buf[512]; if (len > 511) len = 511; memcpy(buf, t, (size_t)len); buf[len] = 0; draw_text_plain(s, x, y, g_rdr_fs, buf, rgb); return; }
    int base = y + g_ttf.ascent + ((rdr_lineh() - (g_ttf.ascent - g_ttf.descent)) / 2);
    ttf_draw_run(s, &g_ttf, x, base, y, t, len, rgb);
}
#endif
