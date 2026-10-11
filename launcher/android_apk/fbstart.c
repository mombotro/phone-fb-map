// fbstart.c - start the app picker detached from whoever runs it. Android's "stop" kills the whole process group of
// the program that asked for it, and an app that runs su is in that group. So: fork, setsid, run the script, exit.
#include <fcntl.h>
#include <unistd.h>
static __thread char tls_pad[32] __attribute__((aligned(32)));
int main(void) {
    tls_pad[0] = 1;
    if (access("/data/local/tmp/run_launcher.sh", R_OK)) return 1;
    pid_t p = fork();
    if (p < 0) return 1;
    if (p > 0) return 0;
    setsid();
    if (fork() != 0) _exit(0);
    int fd = open("/dev/null", O_RDWR);
    if (fd >= 0) { dup2(fd, 0); dup2(fd, 1); dup2(fd, 2); }
    execl("/system/bin/sh", "sh", "/data/local/tmp/run_launcher.sh", "3600", "fblauncher", (char *)NULL);
    _exit(127);
}
