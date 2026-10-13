// lhemoji.h - the emoji of the Longhouse app: 12 small pixel pictures, and the text work around them.
// The font is ASCII only, so an emoji in a message becomes a marker byte (1 to 12) followed by a space; the drawing code puts the picture
// over those two character cells. Emoji that are not in the set stay "?". To type one, the picker puts a short code such as :heart: into
// the text, and the code becomes the real emoji when the message is sent. The first six are the six reactions of Longhouse, in its order.
#ifndef LHEMOJI_H
#define LHEMOJI_H
#include <stdint.h>
#include <string.h>
#include "draw.h"

#define LHE_N 12
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

// A message body from the server: emoji we know become a marker and a space, carriage returns go, tabs become spaces, the rest that is not ASCII is "?".
static void lhe_clean_body(char *s) {
    unsigned char *r = (unsigned char *)s, *w = r;
    while (*r) {
        if (*r < 0x80) { if (*r == '\r') { r++; continue; } *w++ = *r == '\t' ? ' ' : *r; r++; continue; }
        int len, e = lhe_match(r, &len);
        if (e >= 0) { *w++ = (unsigned char)(e + 1); *w++ = ' '; r += len; continue; }
        if (r[0] == 0xEF && r[1] == 0xB8 && r[2] == 0x8F) { r += 3; continue; }                 // a lone variation selector
        *w++ = '?'; r++; while ((*r & 0xC0) == 0x80) r++;
    }
    *w = 0;
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
        case 'D': return 0xB8860B; default: return 0;
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
static void lhe_draw(Surf *s, int idx, int x, int y, int sc) {
    char grid[12][13];
    if (idx == 0 || idx == 1 || idx == 5 || idx == 11) {
        int k = idx == 0 ? 0 : idx == 1 ? 1 : idx == 5 ? 2 : 3;
        for (int r = 0; r < 12; r++) memcpy(grid[r], LHE_SPRITE[k][r], 13);
    } else {
        int f = idx == 6 ? 0 : idx == 7 ? 1 : idx == 8 ? 2 : idx == 9 ? 3 : idx == 10 ? 4 : idx == 2 ? 5 : idx == 3 ? 6 : 7;
        for (int r = 0; r < 12; r++) memcpy(grid[r], LHE_BASE[r], 13);
        for (const char *p = LHE_FACE_FEAT[f]; p[0] && p[1] && p[2]; p += 3) {
            int px = p[0] >= 'a' ? p[0] - 'a' + 10 : p[0] - '0', py = p[1] >= 'a' ? p[1] - 'a' + 10 : p[1] - '0';
            if (px >= 0 && px < 12 && py >= 0 && py < 12) grid[py][px] = p[2];
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
