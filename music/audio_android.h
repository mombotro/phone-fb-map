// audio_android.h - audio engine of the Android music player. A Java helper (android/Helper.java) plays files with the
// platform MediaPlayer. The helper runs under app_process, which still works while the Android framework is stopped,
// because mediaserver (AudioFlinger) keeps running. This file starts the helper and sends it text commands over a pipe.
// Included by player.c, which defines g_volume, g_playing, g_paused, g_eof, now() and LOG first.
#ifndef AUDIO_ANDROID_H
#define AUDIO_ANDROID_H
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <sys/wait.h>

#define HELPER_JAR "/data/local/tmp/musichelper.jar"

static int g_hin = -1, g_hout = -1;            // pipe to and from the helper
static pid_t g_hpid;
static char g_hbuf[1024]; static int g_hlen;
static long g_pos_ms, g_dur_ms;

// File types the platform MediaPlayer on Android 4.x plays.
static int dec_supported(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return 0;
    static const char *ext[] = { ".mp3", ".m4a", ".aac", ".wav", ".ogg", ".oga", ".flac", ".mid", ".amr", ".3gp", NULL };
    for (int i = 0; ext[i]; i++) if (!strcasecmp(dot, ext[i])) return 1;
    return 0;
}

static void hsend(const char *fmt, ...) {
    if (g_hin < 0) return;
    char line[1200]; va_list ap; va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line - 1, fmt, ap); va_end(ap);
    if (n <= 0) return;
    line[n++] = '\n';
    (void)!write(g_hin, line, (size_t)n);
}

static void hline(const char *l) {
    if (!strncmp(l, "pos ", 4)) { long p = 0, d = 0; if (sscanf(l + 4, "%ld %ld", &p, &d) == 2) { g_pos_ms = p; g_dur_ms = d; } }
    else if (!strcmp(l, "state playing")) { g_playing = 1; g_paused = 0; }
    else if (!strcmp(l, "state paused")) g_paused = 1;
    else if (!strcmp(l, "state ended")) g_eof = 1;
    else if (!strncmp(l, "state error", 11)) { LOG("helper: %s", l); g_eof = 1; }
    else if (strcmp(l, "ready") && strncmp(l, "state stopped", 13)) LOG("helper: %s", l);
}

// Read whatever the helper wrote and handle each complete line. Call once per frame.
static void audio_poll(void) {
    if (g_hout < 0) return;
    for (;;) {
        ssize_t r = read(g_hout, g_hbuf + g_hlen, sizeof g_hbuf - 1 - (size_t)g_hlen);
        if (r <= 0) break;
        g_hlen += (int)r; g_hbuf[g_hlen] = 0;
        char *nl;
        while ((nl = strchr(g_hbuf, '\n'))) {
            *nl = 0; if (nl > g_hbuf && nl[-1] == '\r') nl[-1] = 0;
            hline(g_hbuf);
            g_hlen -= (int)(nl + 1 - g_hbuf); memmove(g_hbuf, nl + 1, (size_t)g_hlen + 1);
        }
        if (g_hlen >= (int)sizeof g_hbuf - 2) g_hlen = 0;
    }
}

static int audio_load(void) {
    signal(SIGPIPE, SIG_IGN);
    int to[2], from[2];
    if (access(HELPER_JAR, R_OK)) { fprintf(stderr, "missing %s\n", HELPER_JAR); return -1; }
    if (pipe(to) || pipe(from)) return -1;
    g_hpid = fork();
    if (g_hpid < 0) return -1;
    if (g_hpid == 0) {
        dup2(to[0], 0); dup2(from[1], 1);
        close(to[0]); close(to[1]); close(from[0]); close(from[1]);
        if (!getenv("ANDROID_ROOT")) setenv("ANDROID_ROOT", "/system", 1);
        if (!getenv("ANDROID_DATA")) setenv("ANDROID_DATA", "/data", 1);
        if (!getenv("LD_LIBRARY_PATH")) setenv("LD_LIBRARY_PATH", "/system/lib", 1);
        execl("/system/bin/app_process", "app_process", "-Djava.class.path=" HELPER_JAR, "/system/bin", "Helper", (char *)NULL);
        _exit(127);
    }
    close(to[0]); close(from[1]);
    g_hin = to[1]; g_hout = from[0];
    fcntl(g_hout, F_SETFL, fcntl(g_hout, F_GETFL) | O_NONBLOCK);
    for (int i = 0; i < 160; i++) {                          // wait up to 8 s for "ready"
        ssize_t r = read(g_hout, g_hbuf + g_hlen, sizeof g_hbuf - 1 - (size_t)g_hlen);
        if (r > 0) { g_hlen += (int)r; g_hbuf[g_hlen] = 0; if (strstr(g_hbuf, "ready")) { g_hlen = 0; LOG("audio helper ready (pid %d)", (int)g_hpid); return 0; } }
        int st; if (waitpid(g_hpid, &st, WNOHANG) == g_hpid) { fprintf(stderr, "audio helper exited early\n"); return -1; }
        usleep(50000);
    }
    fprintf(stderr, "audio helper did not start\n");
    return -1;
}

static void stop_audio(void) {
    hsend("stop");
    g_playing = 0; g_paused = 0; g_eof = 0; g_pos_ms = 0; g_dur_ms = 0;
}

static int play_file(const char *path) {
    if (g_hin < 0) return -1;
    hsend("vol %d", (int)(g_volume * 100));
    hsend("play %s", path);
    g_playing = 1; g_paused = 0; g_eof = 0; g_pos_ms = 0; g_dur_ms = 0;
    LOG("play %s", path);
    return 0;
}

static double position_seconds(void) { return g_pos_ms / 1000.0; }
static double audio_duration(void) { return g_dur_ms / 1000.0; }
static double audio_fraction(void) { return g_dur_ms > 0 ? (double)g_pos_ms / g_dur_ms : 0; }
static void audio_seek_fraction(double f) {
    if (g_dur_ms <= 0) return;
    if (f < 0) f = 0; if (f > 1) f = 1;
    g_pos_ms = (long)(f * g_dur_ms); hsend("seek %ld", g_pos_ms);
}
static void audio_set_paused(int p) { hsend(p ? "pause" : "resume"); g_paused = p; }
static void audio_set_volume(float v) { hsend("vol %d", (int)(v * 100)); }
static void audio_end_info(char *b, size_t n) { snprintf(b, n, "pos %ld of %ld ms", g_pos_ms, g_dur_ms); }
static void audio_info(char *b, size_t n) { snprintf(b, n, "pos_ms=%ld dur_ms=%ld eof=%d", g_pos_ms, g_dur_ms, g_eof); }

static void audio_shutdown(void) {
    hsend("quit");
    if (g_hin >= 0) close(g_hin);
    if (g_hpid > 0) { usleep(200000); kill(g_hpid, SIGKILL); waitpid(g_hpid, NULL, 0); }
}
#endif
