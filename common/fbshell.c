// fbshell.c - shell on the framebuffer: pty + /system/bin/sh, terminal text on top, touch keyboard below.
// usage: fbshell [seconds] [scale]   (defaults 300, 2). Type `exit` to quit.
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <unistd.h>
#include "plat.h"
#include "console.h"
#include "kbd.h"
#include "term.h"
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>
#include <termios.h>

static double now(void) {
    struct timeval tv; gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

#ifdef __APPLE__
#define SHELL_PATH "/bin/bash"
#define SHELL_HOME "/var/root"
#else
#define SHELL_PATH "/system/bin/sh"
#define SHELL_HOME "/data/local/tmp"
#endif

// Open a pty master and return the slave's path in `slave`. Returns master fd or -1.
static int open_pty(char *slave, size_t n) {
#ifdef __APPLE__
    int m = posix_openpt(O_RDWR | O_NOCTTY);
    if (m < 0) { perror("posix_openpt"); return -1; }
    if (grantpt(m) || unlockpt(m)) { perror("grantpt/unlockpt"); return -1; }
    const char *name = ptsname(m);
    if (!name) { perror("ptsname"); return -1; }
    snprintf(slave, n, "%s", name);
    return m;
#else
    int m = open("/dev/ptmx", O_RDWR | O_NOCTTY);
    if (m < 0) { perror("open /dev/ptmx"); return -1; }
    int unlock = 0, ptn = 0;
    if (ioctl(m, TIOCSPTLCK, &unlock) || ioctl(m, TIOCGPTN, &ptn)) { perror("pty ioctl"); return -1; }
    snprintf(slave, n, "/dev/pts/%d", ptn);
    return m;
#endif
}

// Start the shell on a new pty. Returns master fd (non-blocking) or -1.
static int spawn_shell(int rows, int cols, pid_t *pid_out) {
    char slave[64];
    int m = open_pty(slave, sizeof slave);
    if (m < 0) return -1;
    struct winsize ws = { (unsigned short)rows, (unsigned short)cols, 0, 0 };
    ioctl(m, TIOCSWINSZ, &ws);

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return -1; }
    if (pid == 0) {
        setsid();
        int s = open(slave, O_RDWR);
        if (s < 0) _exit(127);
        ioctl(s, TIOCSCTTY, 0);
        dup2(s, 0); dup2(s, 1); dup2(s, 2);
        if (s > 2) close(s);
        close(m);
#ifdef __APPLE__
        char *envp[] = { "TERM=dumb", "HOME=" SHELL_HOME, "USER=root", "SHELL=/bin/bash",
                         "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
                         "PS1=\\w # ", NULL };
#else
        char *envp[] = { "TERM=dumb", "HOME=" SHELL_HOME, "PATH=/system/bin:/system/xbin:/sbin:/vendor/bin",
                         "PS1=# ", "ANDROID_ROOT=/system", "ANDROID_DATA=/data", NULL };
#endif
        chdir(SHELL_HOME);
        execle(SHELL_PATH, "sh", (char *)NULL, envp);
        _exit(127);
    }
    fcntl(m, F_SETFL, fcntl(m, F_GETFL) | O_NONBLOCK);
    *pid_out = pid;
    return m;
}

int main(int argc, char **argv) {
    double seconds = argc > 1 ? atof(argv[1]) : 300;
    int scale = argc > 2 ? atoi(argv[2]) : 2;
    PlatInfo pi;
    if (plat_init(&pi)) return 1;
    if (plat_touch_init(&pi)) return 1;
    int W = pi.w, H = pi.h;

    int key_h = W / 8, kbd_h = key_h * 5 + 6, term_h = H - kbd_h;
    Con c;
    if (con_init(&c, W, term_h, scale)) return 1;
    c.rs = pi.rs; c.gs = pi.gs; c.bs = pi.bs; c.alpha = pi.alpha;
    c.cur_fg = 0xDDDDDD;
    Term tm; memset(&tm, 0, sizeof tm);
    Kbd kb; kbd_init(&kb, 0, term_h, W, kbd_h);

    pid_t pid = 0;
    int master = spawn_shell(c.rows, c.cols, &pid);
    if (master < 0) return 1;
    fprintf(stderr, "terminal %dx%d, keyboard %dpx, shell pid %d\n", c.cols, c.rows, kbd_h, (int)pid);

    int prev[16] = {0};
    double hold_t[16] = {0}, next_rep[16] = {0};
    int dirty = 3, blink = 1, alive = 1;
    double t0 = now(), t_blink = t0;
    double t_exit = 0;

    while (now() - t0 < seconds) {
        double t = now();

        PlatTouch tc[10];
        int n = plat_touch_poll(tc, 10);
        int cur[16] = {0};
        for (int i = 0; i < n; i++) {
            int id = tc[i].id & 15;
            cur[id] = 1;
            if (!prev[id]) {
                int code = kbd_press(&kb, id, tc[i].x, tc[i].y);
                if (code >= 0 && alive) { unsigned char b = (unsigned char)code; (void)!write(master, &b, 1); }
                hold_t[id] = t; next_rep[id] = t + 0.45;
            } else if (kbd_held_code(&kb, id) == K_BKSP && t >= next_rep[id] && alive) {
                unsigned char b = 0x7f; (void)!write(master, &b, 1);
                next_rep[id] = t + 0.07;
            }
        }
        for (int id = 0; id < 16; id++) if (prev[id] && !cur[id]) kbd_release(&kb, id);
        for (int id = 0; id < 16; id++) prev[id] = cur[id];

        if (alive) {
            unsigned char buf[2048];
            ssize_t r = read(master, buf, sizeof buf);
            if (r > 0) { term_feed(&tm, &c, buf, (int)r); dirty = 3; blink = 1; t_blink = t; }
            else if (r == 0 || (r < 0 && errno == EIO)) alive = 0;
            int status;
            if (waitpid(pid, &status, WNOHANG) == pid) alive = 0;
            if (!alive) { con_color(&c, 0xFF8888); con_puts(&c, "\n[shell exited]\n"); dirty = 3; t_exit = t; }
        } else if (t - t_exit > 1.5) break;

        if (t - t_blink >= 0.5) { blink ^= 1; t_blink = t; dirty = 3; }
        if (kb.dirty) { dirty = 3; kb.dirty = 0; }

        if (dirty > 0) {
            int stride;
            uint32_t *px = plat_begin(&stride);
            con_render(&c, px, stride, W, term_h, blink && alive);
            Surf s = { px, stride, W, H, pi.rs, pi.gs, pi.bs, pi.alpha };
            kbd_render(&kb, &s);
            plat_flip();
            dirty--;
        } else {
            usleep(8000);
        }
    }
    if (alive) kill(pid, SIGKILL);
    plat_close();
    return 0;
}
