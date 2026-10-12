// musicstate.h - how the music player, its background part and the picker talk (Android only).
// The music keeps playing after the player screen closes, because the audio and the playlist live in a background process
// (musicplayer --daemon). Two small files link the parts:
//   MUSIC_STATE  written by the background part four times a second: what plays, where, and the modes. Old = the background part is gone.
//   MUSIC_CMD    one command per line, added by the player screen or the picker: play, pause, resume, replay, next, prev, seek, shuffle, repeat, stop, quit.
#ifndef MUSICSTATE_H
#define MUSICSTATE_H
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MUSIC_STATE "/data/local/tmp/music.state"
#define MUSIC_CMD "/data/local/tmp/music.cmd"

typedef struct {
    int alive, playing, paused, shuffle, repeat;        // alive: the file is fresh. repeat: 0 off, 1 folder, 2 song
    long pos_ms, dur_ms;
    char dir[1024], file[256];                          // the folder and the file that play
} MusicState;

static void music_state_write(const MusicState *m) {         // one write, so a reader never sees half of it (no rename: some phones deny it)
    char buf[1500];
    int n = snprintf(buf, sizeof buf, "T %ld\nS %d %d %ld %ld %d %d\nD %s\nF %s\n", (long)time(NULL), m->playing, m->paused, m->pos_ms, m->dur_ms, m->shuffle, m->repeat, m->dir, m->file);
    if (n <= 0 || n >= (int)sizeof buf) return;
    int fd = open(MUSIC_STATE, O_WRONLY | O_CREAT, 0666); if (fd < 0) return;
    (void)!write(fd, buf, (size_t)n); (void)!ftruncate(fd, n); close(fd); chmod(MUSIC_STATE, 0666);
}

// Returns 1 when the background part runs (the file is no more than 5 seconds old).
static int music_state_read(MusicState *m) {
    memset(m, 0, sizeof *m);
    FILE *f = fopen(MUSIC_STATE, "r"); if (!f) return 0;
    char l[1100]; long t = 0;
    while (fgets(l, sizeof l, f)) {
        l[strcspn(l, "\r\n")] = 0;
        if (l[0] == 'T') t = atol(l + 2);
        else if (l[0] == 'S') sscanf(l + 2, "%d %d %ld %ld %d %d", &m->playing, &m->paused, &m->pos_ms, &m->dur_ms, &m->shuffle, &m->repeat);
        else if (l[0] == 'D') snprintf(m->dir, sizeof m->dir, "%s", l[1] ? l + 2 : "");
        else if (l[0] == 'F') snprintf(m->file, sizeof m->file, "%s", l[1] ? l + 2 : "");
    }
    fclose(f);
    long d = (long)time(NULL) - t; if (d < 0) d = -d;
    m->alive = d <= 5;
    if (!m->alive) { m->playing = 0; m->paused = 0; }
    return m->alive;
}

static void music_cmd(const char *fmt, ...) {
    char line[1200]; va_list ap; va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line - 1, fmt, ap); va_end(ap);
    if (n <= 0) return;
    if (n > (int)sizeof line - 2) n = (int)sizeof line - 2;
    line[n++] = '\n';
    int fd = open(MUSIC_CMD, O_WRONLY | O_CREAT | O_APPEND, 0666); if (fd < 0) return;
    (void)!write(fd, line, (size_t)n); close(fd); chmod(MUSIC_CMD, 0666);
}
#endif
