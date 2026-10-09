// fbtools_stub.c - the executable inside FBTools.app, the home-screen icon for the app picker (iPhone).
// It runs as root (setuid), starts the launcher in the background, and exits at once. iOS expects an app to
// finish launching within about 20 seconds, so the launcher must not be this process.
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

int main(void) {
    setuid(0); setgid(0);
    pid_t p = fork();
    if (p < 0) return 1;
    if (p == 0) {
        setsid();
        if (fork() != 0) _exit(0);                              // detach from the app process
        int fd = open("/dev/null", O_RDWR);
        if (fd >= 0) { dup2(fd, 0); dup2(fd, 1); dup2(fd, 2); }
        // Start the launcher unless it is already running (a second tap does nothing).
        execl("/bin/sh", "sh", "-c",
              "killall -0 fblauncher >/dev/null 2>&1 || exec sh /var/root/run_launcher_ios.sh", (char *)NULL);
        _exit(127);
    }
    return 0;
}
