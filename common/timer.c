// timer.c - stopwatch, countdown timer, and alarm clock for the iOS and Android framebuffers.
// usage: timer [seconds]     (default 86400)
// The timer and the alarm only ring while this program runs. The alarm time is saved in the data folder.
#include "plat.h"
#include "draw.h"
#include "kbd.h"
#include "ui.h"
#include "../tic80/audio.h"
#include <time.h>
#include <unistd.h>
#include <math.h>
#include <sys/stat.h>

#ifdef __APPLE__
#define DATA_DIR "/var/root/data/timer"
#else
#define DATA_DIR "/data/local/tmp/data/timer"
#endif
#define SND_RATE 22050
#define MAXLAP 12

enum { T_WATCH, T_TIMER, T_ALARM };
enum { R_NONE, R_TIMER, R_ALARM };

static void mkdirs(const char *p) {
    char tmp[256]; snprintf(tmp, sizeof tmp, "%s", p);
    for (char *c = tmp + 1; *c; c++) if (*c == '/') { *c = 0; mkdir(tmp, 0755); *c = '/'; }
    mkdir(tmp, 0755);
}

static int al_h = 7, al_m = 0, al_on = 0;
static void alarm_load(void) {
    FILE *f = fopen(DATA_DIR "/alarm.txt", "r"); if (!f) return;
    int h, m, o; if (fscanf(f, "%d %d %d", &h, &m, &o) == 3 && h >= 0 && h < 24 && m >= 0 && m < 60) { al_h = h; al_m = m; al_on = o != 0; }
    fclose(f);
}
static void alarm_save(void) {
    FILE *f = fopen(DATA_DIR "/alarm.txt", "w"); if (!f) return;
    fprintf(f, "%d %d %d\n", al_h, al_m, al_on); fclose(f);
}

#ifndef __APPLE__
// Android: the alarm is a small WAV file that the Java audio helper (music/android/Helper.java, started with app_process) plays in a
// loop through the system MediaPlayer. The helper starts when the program starts, and the WAV file is written once.
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#define ALARM_WAV "/data/local/tmp/alarm.wav"
#define HELPER_JAR "/data/local/tmp/musichelper.jar"
static int g_hin = -1, g_sent;
static pid_t g_hpid;

static void alarm_wav_write(void) {                                    // 1.6 s: three beeps of 0.2 s, then a pause. 22050 Hz, mono, 16 bit
    if (access(ALARM_WAV, R_OK) == 0) return;
    FILE *f = fopen(ALARM_WAV, "wb"); if (!f) return;
    const int rate = 22050, n = rate * 16 / 10;
    uint32_t data = (uint32_t)n * 2, riff = 36 + data, fmt = 16, byterate = rate * 2;
    uint16_t pcm = 1, ch = 1, align = 2, bits = 16; uint32_t rt = rate;
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f); fwrite(&fmt, 4, 1, f);
    fwrite(&pcm, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&rt, 4, 1, f); fwrite(&byterate, 4, 1, f);
    fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f); fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
    for (int i = 0; i < n; i++) {
        double t = (double)i / rate;
        int beep = t < 1.05 && fmod(t, 0.35) < 0.2;
        int16_t v = beep ? (int16_t)(16000 * sin(2 * M_PI * 988.0 * t)) : 0;
        fwrite(&v, 2, 1, f);
    }
    fclose(f);
    chmod(ALARM_WAV, 0666);
}
static void alarm_helper_start(void) {
    if (access(HELPER_JAR, R_OK) != 0) return;                         // no helper installed: the alarm stays silent
    alarm_wav_write();
    int to[2]; signal(SIGPIPE, SIG_IGN);
    if (pipe(to)) return;
    g_hpid = fork();
    if (g_hpid < 0) return;
    if (g_hpid == 0) {
        dup2(to[0], 0); close(to[0]); close(to[1]);
        int nul = open("/dev/null", O_RDWR); if (nul >= 0) { dup2(nul, 1); dup2(nul, 2); }
        if (!getenv("ANDROID_ROOT")) setenv("ANDROID_ROOT", "/system", 1);
        if (!getenv("ANDROID_DATA")) setenv("ANDROID_DATA", "/data", 1);
        const char *ap = "/system/bin/app_process";
        if (access("/system/bin/app_process64", X_OK) == 0 && access("/system/lib64", F_OK) == 0) {
            ap = "/system/bin/app_process64"; setenv("LD_LIBRARY_PATH", "/system/lib64", 1);
        }
        execl(ap, "app_process", "-Djava.class.path=" HELPER_JAR, "/system/bin", "Helper", (char *)NULL);
        _exit(127);
    }
    close(to[0]); g_hin = to[1];
}
static void alarm_helper_send(const char *line) { if (g_hin >= 0) (void)!write(g_hin, line, strlen(line)); }
static void alarm_helper_stop(void) {
    alarm_helper_send("quit\n");
    if (g_hin >= 0) close(g_hin);
    if (g_hpid > 0) { usleep(200000); kill(g_hpid, SIGKILL); waitpid(g_hpid, NULL, 0); }
}
#endif

// Fill the sound ring with a three-beep pattern while ringing (iOS). On Android, start and stop the looped alarm in the helper.
static void ring_sound(int on) {
#ifdef __APPLE__
    static unsigned long pos;
    if (!snd_q) return;
    while (on && snd_w - snd_r < 4096) {
        int16_t buf[512 * 2];
        for (int i = 0; i < 512; i++, pos++) {
            double t = (double)pos / SND_RATE, c = fmod(t, 1.6);
            int beep = c < 1.05 && fmod(c, 0.35) < 0.2;
            int16_t v = beep ? (int16_t)(9000 * sin(2 * M_PI * 988.0 * t)) : 0;
            buf[2 * i] = buf[2 * i + 1] = v;
        }
        snd_push(buf, 512);
    }
#else
    if (on && !g_sent) { { int mv = plat_volume_get(); char vb[24]; snprintf(vb, sizeof vb, "vol %d\n", mv < 30 ? 30 : mv); alarm_helper_send(vb); alarm_helper_send("loop " ALARM_WAV "\n"); }   /* the alarm never goes below 30% */ g_sent = 1; }
    else if (!on && g_sent) { alarm_helper_send("stop\n"); g_sent = 0; }
#endif
}

static long ceil_l(double x) { long i = (long)x; return x > i ? i + 1 : i; }
static void fmt_hms(char *o, size_t n, long s) { snprintf(o, n, "%02ld:%02ld:%02ld", s / 3600, (s / 60) % 60, s % 60); }
static void fmt_ms(char *o, size_t n, double t) {
    long cs = (long)(t * 100); snprintf(o, n, "%02ld:%02ld.%02ld", cs / 6000, (cs / 100) % 60, cs % 100);
}

static void center(Ui *u, int y, int scale, const char *s, uint32_t rgb) {
    int w = (int)strlen(s) * 8 * scale; draw_text(u->s, (u->W - w) / 2, y, scale, s, rgb);
}

int main(int argc, char **argv) {
    double seconds = argc > 1 ? atof(argv[1]) : 86400;
    PlatInfo pi;
    if (plat_init(&pi)) return 1;
    if (plat_touch_init(&pi)) return 1;
    int W = pi.w, H = pi.h;
    mkdirs(DATA_DIR); alarm_load();
    int have_snd = snd_init(SND_RATE) == 0;
    (void)have_snd;
#ifndef __APPLE__
    alarm_helper_start();
#endif

    Ui u; memset(&u, 0, sizeof u);
    int tab = T_WATCH, quit = 0, ringing = R_NONE, dirty = 3;
    double t0 = ui_now(), ring_t0 = 0, last_wake = 0;

    // stopwatch
    int sw_run = 0; double sw_start = 0, sw_acc = 0; double laps[MAXLAP]; int nlaps = 0;
    // timer
    int tm_set = 300, tm_run = 0; double tm_end = 0, tm_left = 0;     // tm_left: seconds left while paused (0 = idle)
    int al_fired_min = -1;

    while (!quit && ui_now() - t0 < seconds) {
        double now = ui_now();
        PlatTouch tc[10];
        int n = plat_touch_poll(tc, 10);
        ui_poll(&u, tc, n);

        // events
        time_t tt = time(NULL); struct tm lt; localtime_r(&tt, &lt);
        if (tm_run && now >= tm_end && !ringing) { tm_run = 0; tm_left = 0; ringing = R_TIMER; ring_t0 = now; }
        if (al_on && !ringing && lt.tm_hour == al_h && lt.tm_min == al_m && al_fired_min != lt.tm_hour * 60 + lt.tm_min) {
            al_fired_min = lt.tm_hour * 60 + lt.tm_min; ringing = R_ALARM; ring_t0 = now;
            al_on = 0; alarm_save();                                      // an alarm rings once
        }
        if (ringing) {
#ifndef __APPLE__
            plat_screen_wake();                                      // the power button may have turned the screen off: an alarm must wake it
#endif
            ring_sound(1);
            if (now - last_wake > 1.0) { FILE *f = fopen("/tmp/fbwake", "w"); if (f) fclose(f); last_wake = now; }
            dirty = 3;
        }
        static long last_sec; if ((long)now != last_sec) { last_sec = (long)now; dirty = 3; }
        if (sw_run || tm_run) dirty = 3;
        if (u.changed || u.tapped) dirty = 3;
        if (dirty <= 0) { usleep(10000); continue; }
        dirty--;

        int stride; uint32_t *px = plat_begin(&stride);
        Surf s = { px, stride, W, H, pi.rs, pi.gs, pi.bs, pi.alpha, pi.theme };
        u.s = &s; u.W = W; u.H = H;
        int flash = ringing && ((int)((now - ring_t0) * 3) & 1);
        draw_rect(&s, 0, 0, W, H, ringing ? (flash ? 0x8A2A2A : 0x3A1010) : 0x10161E);

        if (ringing) {
            center(&u, 140, 6, ringing == R_ALARM ? "ALARM" : "TIME UP", 0xFFFFFF);
            char c[16]; snprintf(c, sizeof c, "%02d:%02d", lt.tm_hour, lt.tm_min); center(&u, 260, 8, c, 0xFFE0A0);
            if (ui_button(&u, 40, H / 2 + 60, W - 80, 220, "STOP", 0x2E7D4F)) { ringing = R_NONE; ring_sound(0); dirty = 3; }
            plat_flip();
            continue;
        }

        // tab bar
        const char *names[3] = { "WATCH", "TIMER", "ALARM" };
        int tw = (W - 16) / 3;
        for (int i = 0; i < 3; i++)
            if (ui_button(&u, 8 + i * tw, 8, tw - 8, 56, names[i], i == tab ? 0x34608F : 0x26303E)) tab = i;

        char buf[64];
        if (tab == T_WATCH) {
            double el = sw_acc + (sw_run ? now - sw_start : 0);
            fmt_ms(buf, sizeof buf, el); center(&u, 120, 8, buf, 0xFFFFFF);
            int bw = (W - 32) / 3 - 8;
            if (ui_button(&u, 16, 260, bw, 90, sw_run ? "STOP" : "START", sw_run ? 0x883333 : 0x2E7D4F)) {
                if (sw_run) { sw_acc += now - sw_start; sw_run = 0; } else { sw_start = now; sw_run = 1; }
            }
            if (ui_button(&u, 16 + bw + 8, 260, bw, 90, "LAP", 0x34608F) && sw_run && nlaps < MAXLAP) laps[nlaps++] = el;
            if (ui_button(&u, 16 + 2 * (bw + 8), 260, bw, 90, "RESET", 0x6E3AA5)) { sw_run = 0; sw_acc = 0; nlaps = 0; }
            for (int i = 0; i < nlaps; i++) {
                int k = nlaps - 1 - i;
                double split = laps[k] - (k ? laps[k - 1] : 0);
                char a[16], b[16]; fmt_ms(a, sizeof a, laps[k]); fmt_ms(b, sizeof b, split);
                snprintf(buf, sizeof buf, "%2d  %s  +%s", k + 1, a, b);
                ui_label(&u, 24, 380 + i * 36, 2, buf, 0xC8D0DC);
            }
        } else if (tab == T_TIMER) {
            long show = tm_run ? ceil_l(tm_end - now) : (tm_left > 0 ? ceil_l(tm_left) : tm_set);
            if (show < 0) show = 0;
            fmt_hms(buf, sizeof buf, show); center(&u, 120, 8, buf, 0xFFFFFF);
            int idle = !tm_run && tm_left <= 0;
            if (idle) {
                static const struct { const char *l; int d; } adj[6] = { {"H+", 3600}, {"M+", 60}, {"S+", 10}, {"H-", -3600}, {"M-", -60}, {"S-", -10} };
                int bw = (W - 32) / 3 - 8;
                for (int i = 0; i < 6; i++)
                    if (ui_button(&u, 16 + (i % 3) * (bw + 8), 250 + (i / 3) * 76, bw, 68, adj[i].l, 0x26303E)) {
                        tm_set += adj[i].d; if (tm_set < 10) tm_set = 10; if (tm_set > 99 * 3600) tm_set = 99 * 3600;
                    }
                static const struct { const char *l; int v; } pre[3] = { {"1 MIN", 60}, {"5 MIN", 300}, {"10 MIN", 600} };
                for (int i = 0; i < 3; i++)
                    if (ui_button(&u, 16 + i * (bw + 8), 410, bw, 68, pre[i].l, 0x34608F)) tm_set = pre[i].v;
            }
            int bw2 = (W - 40) / 2;
            if (ui_button(&u, 16, 520, bw2, 100, tm_run ? "PAUSE" : "START", tm_run ? 0x883333 : 0x2E7D4F)) {
                if (tm_run) { tm_left = tm_end - now; tm_run = 0; }
                else { tm_end = now + (tm_left > 0 ? tm_left : tm_set); tm_run = 1; }
            }
            if (ui_button(&u, 24 + bw2, 520, bw2, 100, "RESET", 0x6E3AA5)) { tm_run = 0; tm_left = 0; }
        } else {
            snprintf(buf, sizeof buf, "now %02d:%02d:%02d", lt.tm_hour, lt.tm_min, lt.tm_sec); center(&u, 90, 3, buf, 0x8899AA);
            snprintf(buf, sizeof buf, "%02d:%02d", al_h, al_m); center(&u, 150, 10, buf, al_on ? 0xFFE0A0 : 0x8899AA);
            int bw = (W - 32) / 4 - 8;
            static const struct { const char *l; int h, m; } adj[8] = { {"H+",1,0}, {"H-",-1,0}, {"M+5",0,5}, {"M-5",0,-5}, {"H+3",3,0}, {"H-3",-3,0}, {"M+1",0,1}, {"M-1",0,-1} };
            for (int i = 0; i < 8; i++)
                if (ui_button(&u, 16 + (i % 4) * (bw + 8), 340 + (i / 4) * 76, bw, 68, adj[i].l, 0x26303E)) {
                    al_h = ((al_h + adj[i].h) % 24 + 24) % 24; al_m = ((al_m + adj[i].m) % 60 + 60) % 60; alarm_save();
                }
            if (ui_button(&u, 16, 520, W - 32, 100, al_on ? "ALARM ON" : "ALARM OFF", al_on ? 0x2E7D4F : 0x883333)) { al_on = !al_on; al_fired_min = -1; alarm_save(); }
            ui_label(&u, 24, 640, 2, "Rings only while this app is open.", 0x778899);
        }
        if (ui_button(&u, 8, H - 60, W - 16, 52, "EXIT", 0x883333)) quit = 1;
        plat_flip();
    }
    ring_sound(0); snd_close();
#ifndef __APPLE__
    alarm_helper_stop();
#endif
    plat_close();
    return 0;
}
