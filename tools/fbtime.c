// fbtime.c - set the clock of an Android phone. usage: fbtime UNIX_SECONDS
// Sets the system clock and the hardware clock (/dev/rtc0), so the time survives a restart. Needs root.
#include <fcntl.h>
#include <linux/rtc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static __thread char tls_pad[32] __attribute__((aligned(32)));      // keeps the static NDK startup happy (see fbrun.c)

int main(int argc, char **argv) {
    tls_pad[0] = 1;
    if (argc < 2) { fprintf(stderr, "usage: fbtime UNIX_SECONDS\n"); return 2; }
    long long t = atoll(argv[1]);
    if (t < 1700000000LL) { fprintf(stderr, "that is not a time after 2023\n"); return 2; }
    struct timeval tv = { (time_t)t, 0 };
    if (settimeofday(&tv, NULL)) { perror("settimeofday"); return 1; }
    int rt = open("/dev/rtc0", O_RDWR);
    if (rt >= 0) {
        struct tm g; time_t tt = (time_t)t; gmtime_r(&tt, &g);
        struct rtc_time r; memset(&r, 0, sizeof r);
        r.tm_sec = g.tm_sec; r.tm_min = g.tm_min; r.tm_hour = g.tm_hour; r.tm_mday = g.tm_mday; r.tm_mon = g.tm_mon; r.tm_year = g.tm_year;
        if (ioctl(rt, RTC_SET_TIME, &r)) perror("rtc");
        close(rt);
    }
    printf("clock set\n");
    return 0;
}
