// ios_compat.h - clock_gettime and timespec_get for the iOS 9.3 SDK (they arrived in iOS 10). Used for the sources of QuickJS and pocketpy that read the clock.
#ifndef IOS_COMPAT_H
#define IOS_COMPAT_H
#include <sys/time.h>
#include <time.h>
#ifndef CLOCK_MONOTONIC
typedef int clockid_t;
#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1
#define CLOCK_PROCESS_CPUTIME_ID 2
static inline int clock_gettime(clockid_t c, struct timespec *ts) { (void)c; struct timeval tv; gettimeofday(&tv, 0); ts->tv_sec = tv.tv_sec; ts->tv_nsec = tv.tv_usec * 1000; return 0; }
#endif
#ifndef TIME_UTC
#define TIME_UTC 1
static inline int timespec_get(struct timespec *ts, int base) { (void)base; return clock_gettime(CLOCK_REALTIME, ts) == 0 ? base : 0; }
#endif
#endif
