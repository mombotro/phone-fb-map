// lhemoji.h - the emoji of the Longhouse app: 12 small pixel pictures, and the text work around them.
// The font is ASCII only, so an emoji in a message becomes a marker byte (LHE_M0 + the number of the picture) followed by a space; the drawing code puts the picture
// over those two character cells. Emoji that are not in the set stay "?". To type one, the picker puts a short code such as :heart: into
// the text, and the code becomes the real emoji when the message is sent. The first six are the six reactions of Longhouse, in its order.
#ifndef LHEMOJI_H
#define LHEMOJI_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "draw.h"

#define LHE_N 12
#define LHE_M0 16                                                            // the marker of the emoji with colour picture number e is LHE_M0 + e (never a tab, newline or carriage return)
#define LHE_REACT 6

typedef struct {
    const char *code;           // what is typed, between colons
    const char *utf8;           // the emoji as the server sends it (the first bytes are enough to recognize it)
    const char *json;           // the same as JSON escapes, for sending
} LheEmoji;

static const LheEmoji LHE[LHE_N] = {
    { ":thumbs:", "\xF0\x9F\x91\x8D", "\\ud83d\\udc4d" },
    { ":heart:",  "\xE2\x9D\xA4",     "\\u2764\\ufe0f" },
    { ":joy:",    "\xF0\x9F\x98\x82", "\\ud83d\\ude02" },
    { ":wow:",    "\xF0\x9F\x98\xAE", "\\ud83d\\ude2e" },
    { ":cry:",    "\xF0\x9F\x98\xA2", "\\ud83d\\ude22" },
    { ":pray:",   "\xF0\x9F\x99\x8F", "\\ud83d\\ude4f" },
    { ":grin:",   "\xF0\x9F\x98\x80", "\\ud83d\\ude00" },
    { ":smile:",  "\xF0\x9F\x98\x8A", "\\ud83d\\ude0a" },
    { ":wink:",   "\xF0\x9F\x98\x89", "\\ud83d\\ude09" },
    { ":love:",   "\xF0\x9F\x98\x8D", "\\ud83d\\ude0d" },
    { ":cool:",   "\xF0\x9F\x98\x8E", "\\ud83d\\ude0e" },
    { ":party:",  "\xF0\x9F\x8E\x89", "\\ud83c\\udf89" },
};

// Which emoji starts at s (0 to 11), or -1. *len gets the number of bytes it uses, including a variation selector or skin tone after it.
static int lhe_match(const unsigned char *s, int *len) {
    for (int i = 0; i < LHE_N; i++) {
        size_t n = strlen(LHE[i].utf8);
        if (memcmp(s, LHE[i].utf8, n) == 0) {
            size_t k = n;
            if (s[k] == 0xEF && s[k + 1] == 0xB8 && s[k + 2] == 0x8F) k += 3;                                     // variation selector 16
            if (s[k] == 0xF0 && s[k + 1] == 0x9F && s[k + 2] == 0x8F && s[k + 3] >= 0xBB && s[k + 3] <= 0xBF) k += 4;       // skin tone
            *len = (int)k; return i;
        }
    }
    return -1;
}

// ---- every other emoji: drawn from a monochrome emoji font (Noto Emoji, tools/emoji/, installed as emoji.ttf in the data folder of the app) in the
// colour of the text. The text of a message gets a marker (LHE_MARK) and a space for each of them, and the code point goes into a list of the message.
#ifdef STB_TRUETYPE_IMPLEMENTATION
#define LHE_FONT 1
static unsigned char *lhe_fd; static stbtt_fontinfo lhe_fi; static int lhe_fok;
static int lhe_font_open(const char *path) {
    if (lhe_fok) return 0;
    FILE *f = fopen(path, "rb"); if (!f) return -1;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n < 1000 || n > (8L << 20)) { fclose(f); return -1; }
    lhe_fd = (unsigned char *)malloc((size_t)n);
    if (!lhe_fd || fread(lhe_fd, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(lhe_fd); lhe_fd = NULL; return -1; }
    fclose(f);
    if (!stbtt_InitFont(&lhe_fi, lhe_fd, stbtt_GetFontOffsetForIndex(lhe_fd, 0))) { free(lhe_fd); lhe_fd = NULL; return -1; }
    lhe_fok = 1; return 0;
}
static int lhe_has_glyph(int cp) { return lhe_fok && stbtt_FindGlyphIndex(&lhe_fi, cp) != 0; }
typedef struct { int cp, w, h; unsigned char *bm; } LheGlyph;
static LheGlyph lhe_cache[128]; static int lhe_ncache;
static LheGlyph *lhe_glyph(int cp, int px) {
    for (int i = 0; i < lhe_ncache; i++) if (lhe_cache[i].cp == cp) return &lhe_cache[i];
    LheGlyph *g = &lhe_cache[lhe_ncache < 128 ? lhe_ncache++ : (rand() & 127)];
    if (g->bm) stbtt_FreeBitmap(g->bm, NULL);
    int xo, yo; g->cp = cp; g->bm = stbtt_GetCodepointBitmap(&lhe_fi, 0, stbtt_ScaleForMappingEmToPixels(&lhe_fi, (float)px), cp, &g->w, &g->h, &xo, &yo);
    return g;
}
// Draw the glyph of the code point centered in a box of size by size pixels at x, y, in the colour rgb (mapped by the theme like text).
static void lhe_draw_cp(Surf *s, int cp, int x, int y, int size, uint32_t rgb) {
    if (!lhe_fok) return;
    LheGlyph *g = lhe_glyph(cp, size * 100 / 105);
    { static int dbg = -1; if (dbg < 0) dbg = getenv("LHE_DEBUG") != NULL; if (dbg) fprintf(stderr, "draw U+%X at %d,%d size %d bitmap %p %dx%d\n", cp, x, y, size, (void *)g->bm, g->w, g->h); }
    if (!g->bm) return;
    uint32_t c = s->theme ? draw_theme_color(s->theme, rgb) : rgb;
    int cr = (c >> 16) & 255, cg = (c >> 8) & 255, cb = c & 255;
    int ox = x + (size - g->w) / 2, oy = y + (size - g->h) / 2;
    for (int yy = 0; yy < g->h; yy++) {
        int py = oy + yy; if (py < 0 || py >= s->h) continue;
        for (int xx = 0; xx < g->w; xx++) {
            int a = g->bm[yy * g->w + xx], px = ox + xx; if (!a || px < 0 || px >= s->w) continue;
            uint32_t *d = &s->px[(size_t)py * s->stride + px];
            int dr = (*d >> s->rs) & 255, dg = (*d >> s->gs) & 255, db = (*d >> s->bs) & 255;
            *d = (uint32_t)((dr * (255 - a) + cr * a) / 255) << s->rs | (uint32_t)((dg * (255 - a) + cg * a) / 255) << s->gs |
                 (uint32_t)((db * (255 - a) + cb * a) / 255) << s->bs | s->alpha;
        }
    }
}
#else
#define LHE_FONT 0
static int lhe_has_glyph(int cp) { (void)cp; return 0; }
#endif

#define LHE_MAXEMO 24
#define LHE_MARK (LHE_M0 + LHE_N)                                           // the marker byte of an emoji drawn from the font

// The code point at r (UTF-8), and its length in bytes. A bad byte gives 0xFFFD and length 1.
static int lhe_cp(const unsigned char *r, int *len) {
    int c = r[0];
    if (c < 0x80) { *len = 1; return c; }
    if ((c & 0xE0) == 0xC0 && (r[1] & 0xC0) == 0x80) { *len = 2; return (c & 31) << 6 | (r[1] & 63); }
    if ((c & 0xF0) == 0xE0 && (r[1] & 0xC0) == 0x80 && (r[2] & 0xC0) == 0x80) { *len = 3; return (c & 15) << 12 | (r[1] & 63) << 6 | (r[2] & 63); }
    if ((c & 0xF8) == 0xF0 && (r[1] & 0xC0) == 0x80 && (r[2] & 0xC0) == 0x80 && (r[3] & 0xC0) == 0x80) { *len = 4; return (c & 7) << 18 | (r[1] & 63) << 12 | (r[2] & 63) << 6 | (r[3] & 63); }
    *len = 1; return 0xFFFD;
}
static int lhe_is_emoji(int cp) {
    return (cp >= 0x1F000 && cp <= 0x1FAFF) || (cp >= 0x2600 && cp <= 0x27BF) || (cp >= 0x2B00 && cp <= 0x2BFF) || (cp >= 0x2300 && cp <= 0x23FF) ||
           (cp >= 0x2190 && cp <= 0x21FF) || (cp >= 0x25A0 && cp <= 0x25FF) || cp == 0xA9 || cp == 0xAE || cp == 0x2122 || cp == 0x203C || cp == 0x2049 ||
           cp == 0x3030 || cp == 0x303D || cp == 0x3297 || cp == 0x3299 || (cp >= 0x2934 && cp <= 0x2935) || cp == 0x2B50;
}
static int lhe_is_joiner_part(int cp) {                                       // follows an emoji and is part of the same picture
    return cp == 0xFE0F || cp == 0xFE0E || cp == 0x20E3 || (cp >= 0x1F3FB && cp <= 0x1F3FF) || (cp >= 0xE0020 && cp <= 0xE007F);
}

// A message body from the server. ASCII stays (a carriage return goes, a tab becomes a space). The 12 emoji with colour pictures become a marker (LHE_M0 + number)
// and a space. Any other emoji that the emoji font has becomes LHE_MARK and a space, and its code point goes into cps (up to maxcp). The rest is "?".
// A picture made of several (a family, a flag, a skin tone, a joined sequence) is drawn as its first part.
static void lhe_clean_body(char *s, int *cps, int *ncp, int maxcp) {
    unsigned char *r = (unsigned char *)s, *w = r;
    int n = 0; static int dbg = -1; if (dbg < 0) dbg = getenv("LHE_DEBUG") != NULL;
    while (*r) {
        if (*r < 0x80) { if (*r == '\r') { r++; continue; } *w++ = *r == '\t' ? ' ' : *r; r++; continue; }
        int len, e = lhe_match(r, &len);
        if (e >= 0) { *w++ = (unsigned char)(LHE_M0 + e); *w++ = ' '; r += len; continue; }
        int cp = lhe_cp(r, &len);
        if (dbg) fprintf(stderr, "emoji? U+%X emoji=%d font=%d glyph=%d n=%d\n", cp, lhe_is_emoji(cp), LHE_FONT ? lhe_has_glyph(cp) : -1, lhe_has_glyph(cp), n);
        if (cp == 0xFE0F || cp == 0x200D) { r += len; continue; }                 // a lone variation selector or joiner
        if (lhe_is_emoji(cp) && n < maxcp && lhe_has_glyph(cp)) {
            if (cps) cps[n] = cp; n++;
            *w++ = LHE_MARK; *w++ = ' '; r += len;
            int more = 1;                                                         // the parts that belong to it
            while (more && *r >= 0x80) {
                int l2, c2 = lhe_cp(r, &l2);
                if (lhe_is_joiner_part(c2)) r += l2;
                else if (c2 == 0x200D) { r += l2; if (*r >= 0x80) { int l3; lhe_cp(r, &l3); r += l3; } }     // joined with the next picture: leave that out
                else if (cp >= 0x1F1E6 && cp <= 0x1F1FF && c2 >= 0x1F1E6 && c2 <= 0x1F1FF) { r += l2; cp = 0; }      // the second half of a flag
                else more = 0;
            }
            continue;
        }
        *w++ = '?'; r += len;
    }
    *w = 0;
    if (ncp) *ncp = n;
}

// A text to send, as the inside of a JSON string: quotes and newlines escaped, :codes: become the emoji, other non-ASCII becomes a space.
static void lhe_json_body(const char *s, char *out, size_t n) {
    size_t o = 0;
    while (*s && o + 16 < n) {
        if (*s == ':') {
            int hit = -1;
            for (int i = 0; i < LHE_N; i++) if (strncmp(s, LHE[i].code, strlen(LHE[i].code)) == 0) { hit = i; break; }
            if (hit >= 0) { size_t l = strlen(LHE[hit].json); memcpy(out + o, LHE[hit].json, l); o += l; s += strlen(LHE[hit].code); continue; }
        }
        unsigned char c = (unsigned char)*s++;
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
        else if (c == '\n') { out[o++] = '\\'; out[o++] = 'n'; }
        else if (c < 32 || c >= 127) out[o++] = ' ';
        else out[o++] = (char)c;
    }
    out[o] = 0;
}

// ---- the pictures, 12 by 12 pixels. Letters pick a color. Faces start from a round yellow base and have their features poked in.
static uint32_t lhe_color(char c) {
    switch (c) {
        case 'Y': return 0xFFD23C; case 'K': return 0x3A2A10; case 'W': return 0xFFFFFF; case 'R': return 0xE03050; case 'C': return 0x5FD0FF;
        case 'S': return 0xFFC266; case 'B': return 0x3A6FD8; case 'G': return 0x40B860; case 'O': return 0xE08A1E; case 'P': return 0xA050D0;
        case 'D': return 0x9A6B00; default: return 0;
    }
}
static const char *LHE_BASE[12] = {
    "....YYYY....", "..YYYYYYYY..", ".YYYYYYYYYY.", ".YYYYYYYYYY.", "YYYYYYYYYYYY", "YYYYYYYYYYYY",
    "YYYYYYYYYYYY", "YYYYYYYYYYYY", ".YYYYYYYYYY.", ".YYYYYYYYYY.", "..YYYYYYYY..", "....YYYY...." };
// The poked features of the faces: x, y (hex digits, lower case) and the letter, three characters for each point.
static const char *LHE_SPRITE[4][12] = {
    { "............", "......SS....", "......SS....", ".....SSS....", "BB..SSSSSS..", "BB.SSSSSSSS.", "BB.SSSSSSSS.", "BB.SSSSSSS..", "BB.SSSSSSS..", "BB..SSSSS...", "............", "............" },     // thumbs up
    { "............", ".RRR....RRR.", "RRRRR..RRRRR", "RRWRRRRRRRRR", "RRRRRRRRRRRR", "RRRRRRRRRRRR", ".RRRRRRRRRR.", "..RRRRRRRR..", "...RRRRRR...", "....RRRR....", ".....RR.....", "............" },       // heart
    { ".....OO.....", "....SOOS....", "...SSOOSS...", "...SSOOSS...", "..SSSOOSSS..", "..SSSOOSSS..", "..SSSOOSSS..", "...SSOOSS...", "...SSOOSS...", "....BBBB....", "....BBBB....", "............" },    // praying hands
    { "..Y....P....", "...G.R...B..", "O.....Y...G.", ".OO..P......", "..OOO...R...", "...OOOO..Y..", "..OOYOOO....", "..OOOOOO.G..", ".OOROOOOO...", ".OOOOOOOO...", "OOOOOOOOO...", "............" },       // party
};
static const char *LHE_FACE_FEAT[8] = {
    "3" "4" "K3" "5" "K8" "4" "K8" "5" "K2" "7" "K3" "7" "K4" "7" "K5" "7" "K6" "7" "K7" "7" "K8" "7" "K9" "7" "K3" "8" "W4" "8" "W5" "8" "W6" "8" "W7" "8" "W8" "8" "W4" "9" "K5" "9" "K6" "9" "K7" "9" "K",     // grin
    "3" "4" "K3" "5" "K8" "4" "K8" "5" "K2" "7" "K9" "7" "K3" "8" "K8" "8" "K4" "9" "K5" "9" "K6" "9" "K7" "9" "K",                                                                                           // smile
    "3" "4" "K3" "5" "K7" "5" "K8" "5" "K9" "5" "K2" "7" "K9" "7" "K3" "8" "K8" "8" "K4" "9" "K5" "9" "K6" "9" "K7" "9" "K",                                                                                 // wink
    "2" "3" "R4" "3" "R2" "4" "R3" "4" "R4" "4" "R3" "5" "R7" "3" "R9" "3" "R7" "4" "R8" "4" "R9" "4" "R8" "5" "R2" "7" "K9" "7" "K3" "8" "K8" "8" "K4" "9" "K5" "9" "K6" "9" "K7" "9" "K",              // love
    "1" "4" "K2" "4" "K3" "4" "K4" "4" "K5" "4" "K6" "4" "K7" "4" "K8" "4" "K9" "4" "K0" "4" "K2" "5" "K3" "5" "K4" "5" "K7" "5" "K8" "5" "K9" "5" "K2" "7" "K9" "7" "K3" "8" "K8" "8" "K4" "9" "K5" "9" "K6" "9" "K7" "9" "K", // cool
    "2" "3" "K3" "4" "K2" "5" "K9" "3" "K8" "4" "K9" "5" "K2" "7" "K3" "7" "K4" "7" "K5" "7" "K6" "7" "K7" "7" "K8" "7" "K9" "7" "K3" "8" "W4" "8" "W5" "8" "W6" "8" "W7" "8" "W8" "8" "W4" "9" "K5" "9" "K6" "9" "K7" "9" "K0" "6" "C0" "7" "Cb" "6" "Cb" "7" "C", // joy
    "3" "3" "K3" "4" "K8" "3" "K8" "4" "K5" "7" "K6" "7" "K5" "8" "K6" "8" "K5" "9" "K6" "9" "K",                                                                                                           // wow
    "3" "4" "K3" "5" "K8" "4" "K8" "5" "K4" "8" "K5" "8" "K6" "8" "K7" "8" "K3" "9" "K8" "9" "K2" "6" "C2" "7" "C9" "6" "C9" "7" "C",                                                                      // cry
};

// Draw emoji number idx (0 to 11) with its top left corner at x, y; every picture pixel is sc by sc screen pixels.
static void lhe_draw(Surf *s0, int idx, int x, int y, int sc) {
    Surf raw = *s0; raw.theme = 0; Surf *s = &raw;                          // the pictures keep their own colours in every theme
    char grid[12][13];
    int face = !(idx == 0 || idx == 1 || idx == 5 || idx == 11);
    if (!face) {
        int k = idx == 0 ? 0 : idx == 1 ? 1 : idx == 5 ? 2 : 3;
        for (int r = 0; r < 12; r++) memcpy(grid[r], LHE_SPRITE[k][r], 13);
    } else {
        int f = idx == 6 ? 0 : idx == 7 ? 1 : idx == 8 ? 2 : idx == 9 ? 3 : idx == 10 ? 4 : idx == 2 ? 5 : idx == 3 ? 6 : 7;
        for (int r = 0; r < 12; r++) memcpy(grid[r], LHE_BASE[r], 13);
        for (const char *p = LHE_FACE_FEAT[f]; p[0] && p[1] && p[2]; p += 3) {
            int px = p[0] >= 'a' ? p[0] - 'a' + 10 : p[0] - '0', py = p[1] >= 'a' ? p[1] - 'a' + 10 : p[1] - '0';
            if (px >= 0 && px < 12 && py >= 0 && py < 12) grid[py][px] = p[2];
        }
        char edge[12][13]; memcpy(edge, grid, sizeof edge);                 // a dark outline round the face, so that it shows on a pale background
        for (int r = 0; r < 12; r++) for (int c = 0; c < 12; c++) {
            if (edge[r][c] != 'Y') continue;
            if (r == 0 || c == 0 || r == 11 || c == 11 || edge[r - 1][c] == '.' || edge[r + 1][c] == '.' || edge[r][c - 1] == '.' || edge[r][c + 1] == '.') grid[r][c] = 'D';
        }
    }
    for (int r = 0; r < 12; r++) for (int c = 0; c < 12; c++) {
        uint32_t col = lhe_color(grid[r][c]);
        if (col) draw_rect(s, x + c * sc, y + r * sc, sc, sc, col);
    }
}

// The number of the emoji that a reaction string means (0 to 5), or -1.
static int lhe_reaction_index(const char *emoji) {
    int len; int i = lhe_match((const unsigned char *)emoji, &len);
    return i >= 0 && i < LHE_REACT ? i : -1;
}
#endif
