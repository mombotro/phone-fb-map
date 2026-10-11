// plat_android.h - platform layer over /dev/graphics/fb0 (msm fbdev, 3 pages) and evdev touch.
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <linux/rtc.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#ifndef FBIO_WAITFORVSYNC
#define FBIO_WAITFORVSYNC _IOW('F', 32, uint32_t)
#endif

// Raises the TLS segment alignment to 32; bionic static startup rejects 8 on ARM.
// Build with -fno-emulated-tls so this reaches the real TLS segment.
__attribute__((used, aligned(32))) __thread char plat__tls_pad[32];

static struct fb_var_screeninfo plat__v;
static struct fb_fix_screeninfo plat__f;
static uint8_t *plat__base;
static int plat__fd, plat__pages, plat__page;
static double plat__last;
static char plat__bl_path[96];
static int plat__bl_old = -1;

// Best effort: with the framework stopped nobody manages the panel, and a screen that timed out
// before we started stays blank (backlight 0). Unblank it and raise the backlight if it is off.
static void plat__wake_panel(void) {
    // Blank/unblank resets the MDP pipes. Without it a stale SurfaceFlinger overlay or a wedged
    // pipeline can keep the old Android frame on screen while our pans are accepted but invisible.
    ioctl(plat__fd, FBIOBLANK, FB_BLANK_POWERDOWN);
    usleep(300000);
    ioctl(plat__fd, FBIOBLANK, FB_BLANK_UNBLANK);
    static const char *paths[] = { "/sys/class/leds/lcd-backlight/brightness", "/sys/class/leds/backlight/brightness",
                                   "/sys/class/backlight/panel/brightness",
                                   "/sys/class/backlight/lcd-backlight/brightness" };
    for (unsigned i = 0; i < sizeof paths / sizeof paths[0]; i++) {
        int fd = open(paths[i], O_RDWR);
        if (fd < 0) continue;
        char buf[16] = {0};
        int n = read(fd, buf, sizeof buf - 1);
        int old = n > 0 ? atoi(buf) : -1;
        if (old >= 0 && old < 40) {
            lseek(fd, 0, SEEK_SET);
            if (write(fd, "128", 3) == 3) {
                strncpy(plat__bl_path, paths[i], sizeof plat__bl_path - 1);
                plat__bl_old = old;
                fprintf(stderr, "backlight was %d, set to 128 (%s)\n", old, paths[i]);
            }
        }
        close(fd);
        return;
    }
}

static double plat__now(void) {
    struct timeval tv; gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1e6;
}


// ---- the power button turns the screen off and on ----
// With the framework stopped, nothing handles the power key, but it is still an input event. We read it, and switch between
// "backlight 0 and the panel powered down" and "panel on and the backlight back". A flag file tells every program (the picker and the
// tools it starts) that the screen is off. While it is off, touches are ignored and nothing is drawn.
#define PLAT_OFF_FLAG "/data/local/tmp/fbscreen_off"
static int plat__pwr_fd[4], plat__npwr, plat__off, plat__bl_on = 128;
static char plat__bl_use[96];
static double plat__wake_t;

static void plat__bl_find(void) {
    static const char *paths[] = { "/sys/class/leds/lcd-backlight/brightness", "/sys/class/leds/backlight/brightness",
                                   "/sys/class/backlight/panel/brightness", "/sys/class/backlight/lcd-backlight/brightness" };
    for (unsigned i = 0; i < sizeof paths / sizeof paths[0]; i++) {
        int fd = open(paths[i], O_RDWR); if (fd < 0) continue;
        char buf[16] = {0}; int n = (int)read(fd, buf, sizeof buf - 1); close(fd);
        int v = n > 0 ? atoi(buf) : -1;
        snprintf(plat__bl_use, sizeof plat__bl_use, "%s", paths[i]);
        plat__bl_on = v >= 40 ? v : 128;                                   // the level to come back to
        return;
    }
}
static void plat__bl_set(int v) {
    if (!plat__bl_use[0]) return;
    int fd = open(plat__bl_use, O_WRONLY); if (fd < 0) return;
    char b[16]; int l = snprintf(b, sizeof b, "%d", v); (void)!write(fd, b, (size_t)l); close(fd);
}
// The touch buttons under the screen (the LG Intuition has a light behind them) stay lit while the framework is stopped, and the light
// drains the battery. Keep it off while a program runs. The file /data/local/tmp/buttons_on keeps the light as it is. The old brightness
// goes back when the program that started everything ends (Android then lights the buttons itself).
#define PLAT_BTN_LED "/sys/class/leds/button-backlight/brightness"
static int plat__btn_old = -1;
static void plat__buttons_off(int force) {
    static double last;
    double t = plat__now();
    if (!force && t - last < 2) return;
    last = t;
    if (access("/data/local/tmp/buttons_on", F_OK) == 0) return;
    int fd = open(PLAT_BTN_LED, O_RDWR); if (fd < 0) return;
    char b[16] = {0}; int n = (int)read(fd, b, sizeof b - 1), v = n > 0 ? atoi(b) : 0;
    if (v != 0) {
        if (plat__btn_old < 0) plat__btn_old = v;
        lseek(fd, 0, SEEK_SET); (void)!write(fd, "0", 1);
    }
    close(fd);
}
static void plat__buttons_restore(void) {
    if (plat__btn_old <= 0 || getenv("FB_PARENT")) return;                  // a tool started by the picker leaves the light off
    int fd = open(PLAT_BTN_LED, O_WRONLY); if (fd < 0) return;
    char b[16]; int l = snprintf(b, sizeof b, "%d", plat__btn_old); (void)!write(fd, b, (size_t)l); close(fd);
}
static void plat__screen_set(int off);

// Called from plat_touch_poll in the program that shows something. A press of the power key toggles the screen.
static void plat__sync_flag(void) {
    int f = access(PLAT_OFF_FLAG, F_OK) == 0;
    if (f && !plat__off) plat__off = 1;                                   // another program turned the screen off: agree
    else if (!f && plat__off) { plat__off = 0; plat__wake_t = plat__now(); }
}
static void plat__power_events(int toggle) {                             // toggle=0: read and throw away (a program that is waiting)
    struct input_event ev[16]; int pressed = 0;
    for (int i = 0; i < plat__npwr; i++) for (;;) {
        ssize_t n = read(plat__pwr_fd[i], ev, sizeof ev); if (n <= 0) break;
        for (int k = 0; k < n / (int)sizeof ev[0]; k++) if (ev[k].type == EV_KEY && ev[k].code == KEY_POWER && ev[k].value == 1) pressed = 1;
    }
    if (pressed && toggle) plat__screen_set(!plat__off);
}
static void plat__screen_set(int off) {
    if (off == plat__off) return;
    if (off) {
        plat__bl_set(0);
        ioctl(plat__fd, FBIOBLANK, FB_BLANK_POWERDOWN);
        FILE *f = fopen(PLAT_OFF_FLAG, "w"); if (f) fclose(f);
        chmod(PLAT_OFF_FLAG, 0666);
    } else {
        ioctl(plat__fd, FBIOBLANK, FB_BLANK_UNBLANK);
        struct fb_var_screeninfo pv = plat__v; pv.yoffset = (unsigned)(((plat__page + plat__pages - 1) % plat__pages) * (int)plat__v.yres);
        ioctl(plat__fd, FBIOPAN_DISPLAY, &pv);                              // show the last picture again
        plat__bl_set(plat__bl_on);
        plat__buttons_off(1);
        unlink(PLAT_OFF_FLAG);
        plat__wake_t = plat__now();
    }
    plat__off = off;
}

// For programs that must wake the screen themselves (the alarm of the Timer).
PLAT_FN void plat_screen_wake(void) { plat__sync_flag(); if (plat__off) plat__screen_set(0); }

// Rotation for tablets whose screen is wider than tall (the Kindle Fire is 1024x600). The programs are designed for a tall screen,
// so they get a 600x1024 buffer, and flip rotates it into the framebuffer. plat__rot: 0 none, 1 or 3 = the two directions.
// The choice comes from $PLAT_ROTATE, then the file /data/local/tmp/plat_rotate (a single digit), then it is automatic.
static int plat__rot, plat__lw, plat__lh;
static uint32_t *plat__sbuf;          // with no rotation: draw here, copy to the screen page at flip, so nothing shows half drawn
static uint32_t *plat__lbuf;

static int plat__rotation_setting(void) {
    const char *e = getenv("PLAT_ROTATE");
    if (e && *e) return atoi(e);
    FILE *f = fopen("/data/local/tmp/plat_rotate", "r");
    if (f) { int v = -1; if (fscanf(f, "%d", &v) == 1) { fclose(f); return v; } fclose(f); }
    return -1;                                                  // not set: decide from the shape of the screen
}

// The battery level (0 to 100) and whether it charges. Returns -1 when the phone shows no battery.
static int plat_battery(int *pct, int *charging) {
    DIR *d = opendir("/sys/class/power_supply"); if (!d) return -1;
    struct dirent *e; int found = -1;
    while (found < 0 && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[256], v[64];
        snprintf(p, sizeof p, "/sys/class/power_supply/%s/type", e->d_name);
        FILE *f = fopen(p, "r"); if (!f) continue;
        v[0] = 0; if (!fgets(v, sizeof v, f)) v[0] = 0; fclose(f);
        if (!strstr(v, "Battery")) continue;
        snprintf(p, sizeof p, "/sys/class/power_supply/%s/capacity", e->d_name);
        f = fopen(p, "r"); if (!f) continue;
        int c = -1; if (fscanf(f, "%d", &c) != 1) c = -1; fclose(f);
        if (c < 0 || c > 100) continue;
        *pct = c; *charging = 0;
        snprintf(p, sizeof p, "/sys/class/power_supply/%s/status", e->d_name);
        if ((f = fopen(p, "r"))) { v[0] = 0; if (fgets(v, sizeof v, f)) *charging = strstr(v, "Charging") != NULL; fclose(f); }
        found = 0;
    }
    closedir(d);
    return found;
}

// ---- settings the picker can change (Android only). Android itself is stopped while the picker runs, so these use files and commands.
// Master volume, 0 to 100, in /data/local/tmp/volume. The music player and the alarm multiply their own volume by it.
#define PLAT_VOLUME_FILE "/data/local/tmp/volume"
static int plat_volume_get(void) {
    int v = 100; FILE *f = fopen(PLAT_VOLUME_FILE, "r");
    if (f) { if (fscanf(f, "%d", &v) != 1) v = 100; fclose(f); }
    return v < 0 ? 0 : v > 100 ? 100 : v;
}
static void plat_volume_set(int v) {
    FILE *f = fopen(PLAT_VOLUME_FILE, "w"); if (!f) return;
    fprintf(f, "%d\n", v < 0 ? 0 : v > 100 ? 100 : v); fclose(f); chmod(PLAT_VOLUME_FILE, 0666);
}

// The Wi-Fi interface: the first network device with "wlan" in its name (wlan0, tiwlan0). Returns 0 when there is none.
static int plat__wifi_name(char *out, size_t n) {
    DIR *d = opendir("/sys/class/net"); if (!d) return 0;
    struct dirent *e; int ok = 0;
    while (!ok && (e = readdir(d))) if (strstr(e->d_name, "wlan")) { snprintf(out, n, "%s", e->d_name); ok = 1; }
    closedir(d); return ok;
}
// 1 when Wi-Fi is up, 0 when it is down, -1 when the phone has none.
static int plat_wifi_state(void) {
    char nm[32], p[96]; if (!plat__wifi_name(nm, sizeof nm)) return -1;
    snprintf(p, sizeof p, "/sys/class/net/%s/flags", nm);
    FILE *f = fopen(p, "r"); if (!f) return -1;
    unsigned fl = 0; if (fscanf(f, "%x", &fl) != 1) fl = 0; fclose(f);
    return (fl & 1) ? 1 : 0;
}
// Switch the interface down or up. Going down loses the default route, so it is saved first and put back when the link comes up.
static void plat_wifi_set(int on) {
    char nm[32], cmd[256]; if (!plat__wifi_name(nm, sizeof nm)) return;
    static char gw[32] = "";
    if (!on) {
        FILE *f = fopen("/proc/net/route", "r");
        if (f) {
            char line[256], ifn[32]; unsigned dst, g;
            while (fgets(line, sizeof line, f)) if (sscanf(line, "%31s %x %x", ifn, &dst, &g) == 3 && dst == 0 && g) {
                snprintf(gw, sizeof gw, "%u.%u.%u.%u", g & 255, (g >> 8) & 255, (g >> 16) & 255, g >> 24); break;
            }
            fclose(f);
        }
        snprintf(cmd, sizeof cmd, "ifconfig %s down", nm);
    } else if (gw[0]) snprintf(cmd, sizeof cmd, "ifconfig %s up; route add default gw %s dev %s", nm, gw, nm);
    else snprintf(cmd, sizeof cmd, "ifconfig %s up", nm);
    (void)!system(cmd);
}

// Screen brightness as a percent, 5 to 100. Uses the same backlight file as the power button. Returns -1 when none was found.
static int plat_brightness_get(void) {
    if (!plat__bl_use[0]) return -1;
    char mx[128]; snprintf(mx, sizeof mx, "%s", plat__bl_use);
    char *sl = strrchr(mx, '/'); if (!sl) return -1;
    strcpy(sl + 1, "max_brightness");
    int max = 255; FILE *f = fopen(mx, "r"); if (f) { if (fscanf(f, "%d", &max) != 1 || max < 1) max = 255; fclose(f); }
    int v = -1; f = fopen(plat__bl_use, "r"); if (!f) return -1;
    if (fscanf(f, "%d", &v) != 1) v = -1; fclose(f);
    if (v < 0) return -1;
    int pct = v * 100 / max; return pct < 5 ? 5 : pct > 100 ? 100 : pct;
}
static void plat_brightness_set(int pct) {
    if (!plat__bl_use[0]) return;
    char mx[128]; snprintf(mx, sizeof mx, "%s", plat__bl_use);
    char *sl = strrchr(mx, '/'); if (!sl) return;
    strcpy(sl + 1, "max_brightness");
    int max = 255; FILE *f = fopen(mx, "r"); if (f) { if (fscanf(f, "%d", &max) != 1 || max < 1) max = 255; fclose(f); }
    if (pct < 5) pct = 5; if (pct > 100) pct = 100;
    int v = pct * max / 100; if (v < 1) v = 1;
    plat__bl_set(v); plat__bl_on = v;                          // the level the power button brings back
}

// Bluetooth radio switch through rfkill. 1 on, 0 off, -1 when the phone has no Bluetooth switch. This switches the radio only:
// Android is stopped, so nothing pairs or connects until Android runs again.
static int plat__bt_index(void) {
    DIR *d = opendir("/sys/class/rfkill"); if (!d) return -1;
    struct dirent *e; int idx = -1;
    while (idx < 0 && (e = readdir(d))) {
        if (strncmp(e->d_name, "rfkill", 6)) continue;
        char p[128], t[32] = ""; snprintf(p, sizeof p, "/sys/class/rfkill/%s/type", e->d_name);
        FILE *f = fopen(p, "r"); if (!f) continue;
        if (!fgets(t, sizeof t, f)) t[0] = 0; fclose(f);
        if (strncmp(t, "bluetooth", 9) == 0) idx = atoi(e->d_name + 6);
    }
    closedir(d); return idx;
}
static int plat_bt_state(void) {
    int i = plat__bt_index(); if (i < 0) return -1;
    char p[96]; snprintf(p, sizeof p, "/sys/class/rfkill/rfkill%d/state", i);
    FILE *f = fopen(p, "r"); if (!f) return -1;
    int v = -1; if (fscanf(f, "%d", &v) != 1) v = -1; fclose(f);
    return v < 0 ? -1 : v == 1;
}
static void plat_bt_set(int on) {
    int i = plat__bt_index(); if (i < 0) return;
    int fd = open("/dev/rfkill", O_WRONLY);                    // the rfkill device: the way that works on kernels where the state file is read-only
    if (fd >= 0) {
        unsigned char ev[8] = { 0 };
        ev[0] = (unsigned char)(i & 255); ev[1] = (unsigned char)((i >> 8) & 255);   // idx (32 bit, little endian)
        ev[4] = 2; ev[5] = 2; ev[6] = on ? 0 : 1; ev[7] = 0;   // type bluetooth, op change, soft block when off
        (void)!write(fd, ev, sizeof ev); close(fd);
    }
    if (plat_bt_state() != on) {                               // fall back to the state file
        char p[96]; snprintf(p, sizeof p, "/sys/class/rfkill/rfkill%d/state", i);
        FILE *f = fopen(p, "w"); if (f) { fprintf(f, "%d\n", on ? 1 : 0); fclose(f); }
    }
}

// Portrait or landscape: write the rotation file the display layer reads at start. The program must start again to use it.
static void plat_orientation_set(int landscape) {
    int native_wide = plat__v.xres > plat__v.yres;
    int rot = (landscape == native_wide) ? 0 : 1;                // landscape on a wide screen, or portrait on a tall one, needs no rotation
    FILE *f = fopen("/data/local/tmp/plat_rotate", "w"); if (!f) return;
    fprintf(f, "%d\n", rot); fclose(f); chmod("/data/local/tmp/plat_rotate", 0666);
}

// ---- the clock. With Android stopped nothing sets the time from the network, and the phone's battery clock can be far off (the Kindle
// said the year 2000, the Intuition said January 2025). So the picker asks a public time server (NTP, over UDP, by number because the
// phone has no name lookup here), sets the system clock and the hardware clock, and records success in a file.
#define PLAT_TIME_OK "/data/local/tmp/time_ok"
static int plat__ntp_once(const char *ip) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0); if (fd < 0) return -1;
    struct timeval to = { 2, 0 }; setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof to);
    struct sockaddr_in a; memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_port = htons(123); a.sin_addr.s_addr = inet_addr(ip);
    unsigned char pk[48]; memset(pk, 0, sizeof pk); pk[0] = 0x1B;                // version 3, client
    int rc = -1;
    if (sendto(fd, pk, sizeof pk, 0, (struct sockaddr *)&a, sizeof a) == (ssize_t)sizeof pk) {
        unsigned char rp[48];
        if (recv(fd, rp, sizeof rp, 0) == (ssize_t)sizeof rp) {
            unsigned long long secs = ((unsigned long long)rp[40] << 24) | (rp[41] << 16) | (rp[42] << 8) | rp[43];
            unsigned frac = ((unsigned)rp[44] << 24) | (rp[45] << 16) | (rp[46] << 8) | rp[47];
            long long unixs = (long long)secs - 2208988800LL;
            if (unixs > 1700000000LL) {                                          // a sane answer (after 2023)
                struct timeval now; gettimeofday(&now, NULL);
                long long diff = unixs - now.tv_sec; if (diff < 0) diff = -diff;
                if (diff > 2) {
                    struct timeval tv; tv.tv_sec = (time_t)unixs; tv.tv_usec = (suseconds_t)(((unsigned long long)frac * 1000000ULL) >> 32);
                    settimeofday(&tv, NULL);
                    int rt = open("/dev/rtc0", O_RDWR);                          // the battery clock, so a restart keeps the time
                    if (rt >= 0) { struct tm g; time_t t = (time_t)unixs; gmtime_r(&t, &g);
                        struct rtc_time r; memset(&r, 0, sizeof r); r.tm_sec = g.tm_sec; r.tm_min = g.tm_min; r.tm_hour = g.tm_hour; r.tm_mday = g.tm_mday;
                        r.tm_mon = g.tm_mon; r.tm_year = g.tm_year; ioctl(rt, RTC_SET_TIME, &r); close(rt); }
                }
                rc = 0;
            }
        }
    }
    close(fd); return rc;
}
// Try a few servers. Writes PLAT_TIME_OK when one answered.
static void plat__time_sync_now(void) {
    static const char *servers[] = { "216.239.35.0", "129.6.15.28", "162.159.200.1", "132.163.97.1", NULL };
    for (int i = 0; servers[i]; i++) if (plat__ntp_once(servers[i]) == 0) { FILE *f = fopen(PLAT_TIME_OK, "w"); if (f) { fputs("ok\n", f); fclose(f); } return; }
}
// Run the sync in a separate process that is not our child, so the caller's waitpid() calls are not disturbed.
static void plat_time_sync_async(void) {
    pid_t p = fork();
    if (p < 0) return;
    if (p == 0) { if (fork() == 0) { setsid(); plat__time_sync_now(); } _exit(0); }
    int st; waitpid(p, &st, 0);
}
static int plat_time_is_synced(void) { return access(PLAT_TIME_OK, F_OK) == 0; }
static void plat_time_forget(void) { unlink(PLAT_TIME_OK); }
static void plat_timezone_refresh(void);

// A static binary has no Android time zone. Ask the phone's own date command for the offset (like -0500) and use it.
static void plat__timezone(void) {
    if (getenv("TZ")) return;
    FILE *f = popen("date +%z", "r"); if (!f) return;
    char b[16] = ""; if (!fgets(b, sizeof b, f)) b[0] = 0; pclose(f);
    if ((b[0] != '+' && b[0] != '-') || strlen(b) < 5) return;
    int h = (b[1] - '0') * 10 + (b[2] - '0'), m = (b[3] - '0') * 10 + (b[4] - '0');
    char tz[24]; snprintf(tz, sizeof tz, "UTC%c%02d:%02d", b[0] == '+' ? '-' : '+', h, m);   // POSIX sign is reversed
    setenv("TZ", tz, 1); tzset();
}
static void plat_timezone_refresh(void) { unsetenv("TZ"); plat__timezone(); }       // the offset changes with the date (summer time)
static int plat__theme_setting(void) {                      // palm (the default) or dark
    const char *e = getenv("FB_THEME"); char b[16] = "";
    if (!e) { FILE *f = fopen("/data/local/tmp/theme", "r"); if (f) { if (fscanf(f, "%15s", b) == 1) e = b; fclose(f); } }
    return !(e && !strcmp(e, "dark"));
}

PLAT_FN int plat_init(PlatInfo *pi) {
    plat__fd = open("/dev/graphics/fb0", O_RDWR);
    if (plat__fd < 0) plat__fd = open("/dev/fb0", O_RDWR);
    if (plat__fd < 0) { perror("open fb0"); return -1; }
    if (ioctl(plat__fd, FBIOGET_VSCREENINFO, &plat__v) || ioctl(plat__fd, FBIOGET_FSCREENINFO, &plat__f)) {
        perror("ioctl get"); return -1;
    }
    if (plat__v.bits_per_pixel != 32) { fprintf(stderr, "only 32bpp handled\n"); return -1; }
    if (plat__v.yres_virtual / plat__v.yres < 2) {                        // one page only: ask the driver for a second one
        struct fb_var_screeninfo want = plat__v; want.yres_virtual = plat__v.yres * 2;
        if (ioctl(plat__fd, FBIOPUT_VSCREENINFO, &want) == 0) {
            ioctl(plat__fd, FBIOGET_VSCREENINFO, &plat__v); ioctl(plat__fd, FBIOGET_FSCREENINFO, &plat__f);
            fprintf(stderr, "framebuffer: asked for 2 pages, now yres_virtual %u\n", plat__v.yres_virtual);
        }
    }
    plat__pages = plat__v.yres_virtual / plat__v.yres;
    if (plat__pages < 1) plat__pages = 1;
    plat__page = plat__pages > 1 ? 1 : 0;
    plat__base = mmap(NULL, plat__f.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, plat__fd, 0);
    if (plat__base == MAP_FAILED) { perror("mmap"); return -1; }
    if (!getenv("FB_PARENT")) plat__wake_panel();      // the launcher already woke the panel
    pi->name = "Android / fbdev (/dev/graphics/fb0)";
    pi->w = plat__v.xres; pi->h = plat__v.yres;
    int rs = plat__rotation_setting();
    plat__rot = rs >= 0 ? rs : (plat__v.xres > plat__v.yres ? 1 : 0);
    if (plat__rot != 0 && plat__rot != 1 && plat__rot != 3) plat__rot = 0;
    if (plat__rot) {
        plat__lw = plat__v.yres; plat__lh = plat__v.xres;
        plat__lbuf = calloc((size_t)plat__lw * plat__lh, sizeof(uint32_t));
        if (!plat__lbuf) plat__rot = 0;
        else { pi->w = plat__lw; pi->h = plat__lh; fprintf(stderr, "rotation %d: programs see %dx%d, framebuffer is %ux%u\n", plat__rot, plat__lw, plat__lh, plat__v.xres, plat__v.yres); }
    }
    pi->rs = plat__v.red.offset; pi->gs = plat__v.green.offset; pi->bs = plat__v.blue.offset;
    pi->alpha = 0;                          // most fbdev drivers ignore alpha
    // A display with an 8-bit alpha channel (the Kindle Fire's omapfb) can treat alpha 0 as transparent and show nothing.
    if (plat__v.transp.length == 8 && plat__v.transp.offset < 32) pi->alpha = 0xFFu << plat__v.transp.offset;
    // The LG Intuition reports red at bit 24, green 16, blue 8. Its display reads the bytes as R,G,B,A in memory order
    // (a photo of colour bars showed red lost, green and blue swapped), so use the byte order the display really uses.
    if (plat__v.red.offset == 24 && plat__v.green.offset == 16 && plat__v.blue.offset == 8 && plat__v.transp.offset == 0) {
        pi->rs = 0; pi->gs = 8; pi->bs = 16; pi->alpha = 0xFF000000u;
    }
    if (!plat__rot) {                                        // no rotation: still draw in memory and copy to the page at flip, so the page on screen is never half drawn
        plat__sbuf = calloc(1, (size_t)plat__v.yres * plat__f.line_length);
        if (plat__sbuf) fprintf(stderr, "framebuffer: %d pages, drawing in memory and copying at flip\n", plat__pages);
    }
    pi->theme = plat__theme_setting();
    plat__timezone();
    plat__bl_find();                                           // after wake_panel: the level to return to when the screen comes back
    if (!getenv("FB_PARENT")) unlink(PLAT_OFF_FLAG);           // a program started by the launcher keeps the state; a fresh start begins with the screen on
    plat__buttons_off(1);
    plat__last = plat__now();
    return 0;
}

PLAT_FN uint32_t *plat_begin(int *stride_px) {
    if (plat__rot) { *stride_px = plat__lw; return plat__lbuf; }
    *stride_px = plat__f.line_length / 4;
    if (plat__sbuf) return plat__sbuf;
    return (uint32_t *)(plat__base + (size_t)plat__page * plat__v.yres * plat__f.line_length);
}

// Rotate the programs' buffer into the framebuffer page we are about to show.
static void plat__rotate_into_page(void) {
    uint32_t *dst0 = (uint32_t *)(plat__base + (size_t)plat__page * plat__v.yres * plat__f.line_length);
    int fw = plat__v.xres, fh = plat__v.yres, fs = plat__f.line_length / 4, lw = plat__lw, lh = plat__lh;
    for (int py = 0; py < fh; py++) {
        uint32_t *dst = dst0 + (size_t)py * fs;
        if (plat__rot == 1) { int lx = lw - 1 - py; for (int px = 0; px < fw; px++) dst[px] = plat__lbuf[(size_t)px * lw + lx]; }
        else                { int lx = py;          for (int px = 0; px < fw; px++) dst[px] = plat__lbuf[(size_t)(lh - 1 - px) * lw + lx]; }
    }
}

PLAT_FN void plat_flip(void) {
    if (plat__off) { usleep(60000); return; }                  // the screen is off: do not draw
    if (plat__rot) plat__rotate_into_page();
    else if (plat__sbuf) memcpy(plat__base + (size_t)plat__page * plat__v.yres * plat__f.line_length, plat__sbuf, (size_t)plat__v.yres * plat__f.line_length);
    struct fb_var_screeninfo pv = plat__v;
    pv.yoffset = plat__page * plat__v.yres;
    ioctl(plat__fd, FBIOPAN_DISPLAY, &pv);
    uint32_t zero = 0;
    ioctl(plat__fd, FBIO_WAITFORVSYNC, &zero);              // fails on these devices; timer below caps fps
    plat__page = (plat__page + 1) % plat__pages;
    double t = plat__now();
    if (t - plat__last < 1.0 / 60) usleep((useconds_t)((1.0 / 60 - (t - plat__last)) * 1e6));
    plat__last = plat__now();
}

PLAT_FN void plat_close(void) {
    plat__buttons_restore();
    if (plat__off) plat__screen_set(0);                        // never leave the phone with its screen off
    struct fb_var_screeninfo z = plat__v; z.yoffset = 0;
    ioctl(plat__fd, FBIOPAN_DISPLAY, &z);
    if (plat__bl_old >= 0) {                                 // put the backlight back
        int fd = open(plat__bl_path, O_WRONLY);
        if (fd >= 0) { char b[16]; int l = snprintf(b, sizeof b, "%d", plat__bl_old); (void)!write(fd, b, l); close(fd); }
    }
}

// ---- touch (evdev; handles multitouch protocol A and B, and single-touch) ----
#define PLAT_NSLOT 16
typedef struct { int active, x, y; } PlatSlot;
static PlatSlot plat__slots[PLAT_NSLOT], plat__pend[PLAT_NSLOT];
static int plat__tfd = -1, plat__cur, plat__protoA, plat__npend, plat__px, plat__py, plat__phas;
static int plat__mt, plat__minx, plat__maxx, plat__miny, plat__maxy, plat__sw, plat__sh;
static int plat__tportrait, plat__tflip;                          // panel axes are portrait while the screen is landscape; flip bits

static int plat__bit(const unsigned char *b, int n) { return (b[n / 8] >> (n % 8)) & 1; }

PLAT_FN int plat_touch_init(const PlatInfo *pi) {
    (void)pi;
    plat__sw = plat__v.xres; plat__sh = plat__v.yres;                   // touch is measured on the physical screen
    for (int i = 0; i < 32; i++) {
        char path[32]; snprintf(path, sizeof path, "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        unsigned char abs[ABS_MAX / 8 + 1], key[KEY_MAX / 8 + 1];
        memset(abs, 0, sizeof abs); memset(key, 0, sizeof key);
        ioctl(fd, EVIOCGBIT(EV_ABS, sizeof abs), abs);
        ioctl(fd, EVIOCGBIT(EV_KEY, sizeof key), key);
        char name[64] = "?"; ioctl(fd, EVIOCGNAME(sizeof name), name);
        int mt = plat__bit(abs, ABS_MT_POSITION_X) && plat__bit(abs, ABS_MT_POSITION_Y);
        int st = plat__bit(abs, ABS_X) && plat__bit(abs, ABS_Y) && plat__bit(key, BTN_TOUCH);
        if (plat__bit(key, KEY_POWER) && plat__npwr < 4) {                  // a device with a power key: read it too
            int d = dup(fd);
            if (d >= 0) { plat__pwr_fd[plat__npwr++] = d; fprintf(stderr, "power key: %s (%s)\n", path, name); }
        }
        if ((!mt && !st) || plat__tfd >= 0) { close(fd); continue; }       // not a touchscreen, or we already have one
        struct input_absinfo ax, ay;
        ioctl(fd, EVIOCGABS(mt ? ABS_MT_POSITION_X : ABS_X), &ax);
        ioctl(fd, EVIOCGABS(mt ? ABS_MT_POSITION_Y : ABS_Y), &ay);
        fprintf(stderr, "touch: %s (%s) %s x %d..%d y %d..%d\n", path, name, mt ? "multitouch" : "single",
                ax.minimum, ax.maximum, ay.minimum, ay.maximum);
        plat__tfd = fd; plat__mt = mt;
        plat__minx = ax.minimum; plat__maxx = ax.maximum;
        plat__miny = ay.minimum; plat__maxy = ay.maximum;
        plat__tportrait = (plat__maxx - plat__minx) < (plat__maxy - plat__miny) && plat__v.xres > plat__v.yres;
        { int v = 0; const char *e = getenv("PLAT_TOUCH_FLIP");
          if (e) v = atoi(e); else { FILE *f = fopen("/data/local/tmp/plat_touch", "r"); if (f) { if (fscanf(f, "%d", &v) != 1) v = 0; fclose(f); } }
          plat__tflip = v; }
        if (plat__tportrait) fprintf(stderr, "touch panel is portrait-native: flip bits %d\n", plat__tflip);
    }
    if (plat__tfd < 0) { fprintf(stderr, "no touchscreen found in /dev/input\n"); return -1; }
    if (!plat__npwr) fprintf(stderr, "no power key found: the power button cannot turn the screen off\n");
    return 0;
}

PLAT_FN int plat_touch_poll(PlatTouch *out, int max) {
    plat__sync_flag(); plat__power_events(1); plat__buttons_off(0);
    struct input_event ev[64];
    for (;;) {
        ssize_t n = read(plat__tfd, ev, sizeof ev);
        if (n <= 0) break;                                   // EAGAIN: drained
        for (int i = 0; i < n / (int)sizeof(struct input_event); i++) {
            int t = ev[i].type, c = ev[i].code, v = ev[i].value;
            if (t == EV_ABS) {
                if (c == ABS_MT_SLOT) plat__cur = (v >= 0 && v < PLAT_NSLOT) ? v : 0;
                else if (c == ABS_MT_TRACKING_ID) plat__slots[plat__cur].active = v >= 0;
                else if (c == ABS_MT_POSITION_X) { plat__slots[plat__cur].x = plat__px = v; plat__phas |= 1; }
                else if (c == ABS_MT_POSITION_Y) { plat__slots[plat__cur].y = plat__py = v; plat__phas |= 2; }
                else if (!plat__mt && c == ABS_X) plat__slots[0].x = v;
                else if (!plat__mt && c == ABS_Y) plat__slots[0].y = v;
            } else if (t == EV_KEY && c == BTN_TOUCH && !plat__mt) {
                plat__slots[0].active = v;
            } else if (t == EV_SYN && c == 2 /* SYN_MT_REPORT */) {   // protocol A: one contact done
                plat__protoA = 1;
                if (plat__phas == 3 && plat__npend < PLAT_NSLOT) {
                    plat__pend[plat__npend].x = plat__px; plat__pend[plat__npend].y = plat__py;
                    plat__pend[plat__npend].active = 1; plat__npend++;
                }
                plat__phas = 0;
            } else if (t == EV_SYN && c == SYN_REPORT) {
                if (plat__protoA) {                                  // frame complete: contacts = pending list
                    for (int s = 0; s < PLAT_NSLOT; s++) {
                        plat__slots[s].active = s < plat__npend;
                        if (s < plat__npend) { plat__slots[s].x = plat__pend[s].x; plat__slots[s].y = plat__pend[s].y; }
                    }
                    plat__npend = 0;
                }
                plat__phas = 0;
            }
        }
    }
    int cnt = 0;
    if (plat__off || plat__now() - plat__wake_t < 0.4) return 0;         // the screen is off (or just came on): no touches, so a press in a pocket does nothing
    for (int s = 0; s < PLAT_NSLOT && cnt < max; s++) {
        if (!plat__slots[s].active) continue;
        out[cnt].id = s;
        // Raw position as a fraction of the touch panel, 0..1 on each axis.
        double u = (plat__slots[s].x - plat__minx) / (double)(plat__maxx - plat__minx + 1);
        double v = (plat__slots[s].y - plat__miny) / (double)(plat__maxy - plat__miny + 1);
        if (plat__tportrait) {
            // The panel's own axes are portrait (the Kindle Fire: x 0..2305, y 0..3969) although the framebuffer is landscape.
            // Its axes follow the portrait picture. Flip bits come from /data/local/tmp/plat_touch. In portrait mode (rotation on)
            // the programs see that picture. In landscape mode the portrait point is turned a quarter turn to match the screen.
            int pw = (int)plat__sh, ph = (int)plat__sw;                  // the portrait picture: as wide as the screen is tall
            int lx = (int)(u * pw), ly = (int)(v * ph);
            if (plat__tflip & 1) lx = pw - 1 - lx;
            if (plat__tflip & 2) ly = ph - 1 - ly;
            if (plat__tflip & 4) { int t = lx * ph / pw; lx = ly * pw / ph; ly = t; }
            if (plat__rot) { out[cnt].x = lx; out[cnt].y = ly; }
            else { out[cnt].x = ly; out[cnt].y = pw - 1 - lx; }
        } else {
            int px = (int)(u * plat__sw), py = (int)(v * plat__sh);
            if (plat__rot == 1)      { out[cnt].x = plat__lw - 1 - py; out[cnt].y = px; }
            else if (plat__rot == 3) { out[cnt].x = py;               out[cnt].y = plat__lh - 1 - px; }
            else                     { out[cnt].x = px;               out[cnt].y = py; }
        }
        cnt++;
    }
    return cnt;
}

// For a launcher that waits on a child program. Nothing to refresh on Android.
PLAT_FN void plat_keepalive(void) { plat__sync_flag(); plat__power_events(0); plat__buttons_off(0); }
PLAT_FN void plat_emergency_cleanup(void) {}           // nothing to clean up: the display is the plain framebuffer
