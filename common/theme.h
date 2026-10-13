// theme.h - the colour themes of the programs. The programs are drawn in a dark palette (dark background, light text). A theme maps every
// colour of a drawing to a blend of three: PAPER (what a dark colour becomes: the background), MID, and INK (what a light colour becomes: the
// text and the lines). Theme 0 is "dark": the colours as the programs give them. The choice is the name in the file <data>/theme, or $FB_THEME.
#ifndef THEME_H
#define THEME_H
#include <stdint.h>
#include <string.h>

typedef struct { const char *name, *label; uint32_t paper, mid, ink; } Theme;
static const Theme THEMES[] = {
    { "dark",      "Dark",       0x000000, 0x000000, 0x000000 },      // no mapping: the colours as given
    { "palm",      "Palm",       0xC6CFAE, 0x727A62, 0x1F2A1B },      // a grey-green LCD (the first look of the picker)
    { "pink",      "Pink",       0xFFD9EA, 0xE8589B, 0x4A0F3C },
    { "purple",    "Purple",     0xE6D9FF, 0x8C5BD9, 0x2A0F5C },
    { "gameboy",   "Game Boy",   0x9BBC0F, 0x306230, 0x0F380F },
    { "amber",     "Amber CRT",  0x140C00, 0x8A5A00, 0xFFB000 },
    { "green",     "Green CRT",  0x051405, 0x1E8A3C, 0x5CFF8C },
    { "synth",     "Synthwave",  0x1A0B30, 0xFF2E97, 0x22E5FF },
    { "ocean",     "Ocean",      0xD6ECFF, 0x3E7FC1, 0x06264A },
    { "sunset",    "Sunset",     0xFFE3B3, 0xE8602C, 0x3A0A1E },
    { "paper",     "Paper",      0xF4F4F0, 0x8A8A88, 0x101010 },
};
#define THEME_N ((int)(sizeof THEMES / sizeof THEMES[0]))

// The number of a theme by its name. Unknown or missing: 1 (palm), the default.
static int theme_index(const char *name) {
    if (name) for (int i = 0; i < THEME_N; i++) if (!strcmp(name, THEMES[i].name)) return i;
    return 1;
}
#endif
