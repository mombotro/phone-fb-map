// fbrun.c - run an FB Tools program on the Android framebuffer, and always give Android back afterwards.
// It replaces the shell scripts run_launcher.sh, run_boot.sh and fbstart. A C program because Android 2.3's shell has no
// "test" or "[" and crashes on "trap", and because it must keep working when adb drops while Android is stopped.
//
// usage: fbrun [-d] [-k] [-b] [seconds] [program]       (defaults: 3600 s, fblauncher; the program is in /data/local/tmp)
//   -d  detach: run in the background in its own session, so closing adb or the app that started it changes nothing
//   -k  kiosk: set FB_KIOSK=1 (the picker then has no Exit tile)
//   -b  boot loop: wait for boot, then keep the program running. Does nothing if /data/local/tmp/fbboot_off exists.
//       After 3 runs in a row that end within 20 s it stops and leaves Android alone.
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static __thread char tls_pad[32] __attribute__((aligned(32)));      // keeps the static NDK startup happy
#define DIR "/data/local/tmp"
static volatile sig_atomic_t g_stop;
static pid_t g_child;

static void on_signal(int sig) { (void)sig; g_stop = 1; if (g_child > 0) kill(-g_child, SIGTERM); }       // -pid: the whole group (the picker and its tools)
static double now(void) { struct timeval tv; gettimeofday(&tv, NULL); return tv.tv_sec + tv.tv_usec / 1e6; }
static void logline(const char *msg) {
    FILE *f = fopen(DIR "/boot.log", "a");
    if (f) { fprintf(f, "%ld fbrun: %s\n", (long)time(NULL), msg); fclose(f); }
}

// Read a system property through the getprop tool. A static program cannot read the property area of an old Android
// (it has another layout), so __system_property_get returns nothing there.
static int sysprop_is(const char *key, const char *want) {
    char cmd[128], v[128] = "";
    snprintf(cmd, sizeof cmd, "getprop %s 2>/dev/null", key);
    FILE *p = popen(cmd, "r");
    if (!p) return 0;
    (void)!fgets(v, sizeof v, p); pclose(p);
    v[strcspn(v, "\r\n")] = 0;
    return strcmp(v, want) == 0;
}
static int window_up(void) {                                          // "service check window" prints "found" or "not found"
    FILE *p = popen("service check window 2>/dev/null", "r");
    if (!p) return 0;
    char line[256] = ""; (void)!fgets(line, sizeof line, p); pclose(p);
    return strstr(line, "found") && !strstr(line, "not found");
}

// Never stop Android while it is still booting: stopping SurfaceFlinger half way wedges the display.
static void wait_ready(void) {
    for (int i = 0; i < 90 && !g_stop; i++) {
        if (window_up() && sysprop_is("init.svc.bootanim", "stopped")) break;
        sleep(1);
    }
    sleep(3);
}

static void write_sysfs(const char *path, const char *val) {
    int fd = open(path, O_WRONLY);
    if (fd >= 0) { (void)!write(fd, val, strlen(val)); close(fd); }
}
// A blank and unblank before "start" resets the display pipes, so Android comes back on a clean display.
static void give_back_android(void) {
    write_sysfs("/sys/class/graphics/fb0/blank", "4");
    sleep(1);
    write_sysfs("/sys/class/graphics/fb0/blank", "0");
    (void)!system("start");
}

// One run of the program with Android stopped. Returns after Android is back.
static void run_once(const char *prog, long seconds, int kiosk) {
    char path[256], out[256], secs[32];
    snprintf(path, sizeof path, DIR "/%s", prog);
    snprintf(out, sizeof out, DIR "/%s.out", prog);
    snprintf(secs, sizeof secs, "%ld", seconds);
    logline("waiting for Android to finish booting");
    wait_ready();
    if (g_stop) { logline("stopped by a signal before starting"); return; }
    logline("stopping Android and starting the program");
    (void)!system("stop");
    g_child = fork();
    if (g_child == 0) {
        setpgid(0, 0);                                                  // its own group, so everything it starts can be stopped together
        int fd = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); close(fd); }
        close(0);
        if (kiosk) setenv("FB_KIOSK", "1", 1);
        execl(path, prog, secs, (char *)NULL);
        _exit(127);
    }
    double t0 = now();
    int st;
    while (g_child > 0) {
        pid_t r = waitpid(g_child, &st, WNOHANG);
        if (r == g_child) break;
        if (r < 0 && errno != EINTR) break;
        if (g_stop) { kill(-g_child, SIGTERM); usleep(500000); kill(-g_child, SIGKILL); waitpid(g_child, &st, 0); break; }
        if (now() - t0 > seconds + 30) { kill(-g_child, SIGKILL); waitpid(g_child, &st, 0); break; }   // watchdog
        usleep(200000);
    }
    g_child = 0;
    logline("program ended, giving Android back");
    give_back_android();
    logline("start command sent");
}

// Only one fbrun may run: a second tap on the icon, or the boot job and the icon together, must not start two pickers.
static int already_running(void) {
    FILE *f = fopen(DIR "/fbrun.pid", "r");
    if (!f) return 0;
    long pid = 0; int ok = fscanf(f, "%ld", &pid) == 1; fclose(f);
    if (!ok || pid <= 0 || kill((pid_t)pid, 0) != 0) return 0;                     // gone
    char path[64], cmd[256] = ""; snprintf(path, sizeof path, "/proc/%ld/cmdline", pid);
    f = fopen(path, "r"); if (!f) return 0;
    size_t n = fread(cmd, 1, sizeof cmd - 1, f); fclose(f); (void)n;
    return strstr(cmd, "fbrun") != NULL;                                           // alive and really ours (not a reused pid)
}
static void write_pid(void) { FILE *f = fopen(DIR "/fbrun.pid", "w"); if (f) { fprintf(f, "%ld\n", (long)getpid()); fclose(f); } }
static void remove_pid(void) { unlink(DIR "/fbrun.pid"); }

int main(int argc, char **argv) {
    tls_pad[0] = 1;
    logline("started");
    int detach = 0, kiosk = 0, boot = 0, i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        for (const char *c = argv[i] + 1; *c; c++) { if (*c == 'd') detach = 1; else if (*c == 'k') kiosk = 1; else if (*c == 'b') boot = 1; }
    }
    long seconds = i < argc ? atol(argv[i++]) : 3600;
    const char *prog = i < argc ? argv[i] : "fblauncher";
    if (seconds <= 0) seconds = 3600;
    if (boot && access(DIR "/fbboot_off", F_OK) == 0) return 0;

    if (detach) {
        // The program that started us (su, an app) may kill its whole process group the moment we return. So the first
        // process waits until the detached copy is in its own session, and only then returns.
        int pfd[2];
        if (pipe(pfd)) return 1;
        pid_t p = fork();
        if (p < 0) return 1;
        if (p > 0) {
            close(pfd[1]);
            fd_set fds; FD_ZERO(&fds); FD_SET(pfd[0], &fds);
            struct timeval tv = { 5, 0 };
            if (select(pfd[0] + 1, &fds, NULL, NULL, &tv) > 0) { char c; (void)!read(pfd[0], &c, 1); }
            return 0;
        }
        close(pfd[0]);
        setsid();
        if (fork() != 0) { (void)!write(pfd[1], "x", 1); _exit(0); }       // the middle process: the copy has its own session
        close(pfd[1]);
        int fd = open("/dev/null", O_RDWR);
        if (fd >= 0) { dup2(fd, 0); dup2(fd, 1); dup2(fd, 2); }
        logline("detached");
    }
    if (already_running()) { logline("another fbrun is running: leaving"); return 0; }
    write_pid(); atexit(remove_pid);
    signal(SIGTERM, on_signal); signal(SIGINT, on_signal); signal(SIGHUP, on_signal); signal(SIGPIPE, SIG_IGN);

    if (!boot) { run_once(prog, seconds, kiosk); return 0; }
    logline("boot loop starts");
    int fails = 0;
    while (!g_stop && fails < 3 && access(DIR "/fbboot_off", F_OK) != 0) {
        double t0 = now();
        run_once(prog, seconds, kiosk);
        fails = (now() - t0 < 20) ? fails + 1 : 0;
        if (g_stop || access(DIR "/fbboot_off", F_OK) == 0) break;
        sleep(2);
    }
    logline(fails >= 3 ? "gave up after 3 quick failures (Android left running)" : "boot loop ends");
    return 0;
}
