// audio.h - sound output for the TIC-80 player (iOS only). The game thread pushes 16-bit stereo frames
// into a ring buffer, and an AudioQueue callback drains it. Underrun gives silence. Overrun drops frames.
// AudioToolbox loads at run time, so no framework link is needed. On other platforms the functions do nothing.
#ifndef TIC_AUDIO_H
#define TIC_AUDIO_H

#include <stdint.h>

#ifdef __APPLE__
#include <AudioToolbox/AudioToolbox.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

#define SND_RING_FRAMES 8192                       /* power of two, about 186 ms at 44.1 kHz */
#define SND_BUF_FRAMES 1024
#define SND_BUFS 3

static int16_t snd_ring[SND_RING_FRAMES * 2];
static volatile unsigned snd_w, snd_r;             /* frame counters, only ever increase */
static AudioQueueRef snd_q;
static OSStatus (*snd_enqueue)(AudioQueueRef, AudioQueueBufferRef, UInt32, const AudioStreamPacketDescription *);
static OSStatus (*snd_stop)(AudioQueueRef, Boolean);
static OSStatus (*snd_dispose)(AudioQueueRef, Boolean);

static void snd_cb(void *user, AudioQueueRef q, AudioQueueBufferRef b) {
    (void)user;
    int16_t *out = b->mAudioData;
    unsigned frames = b->mAudioDataBytesCapacity / 4;
    unsigned avail = snd_w - snd_r;
    for (unsigned i = 0; i < frames; i++) {
        if (avail) {
            unsigned p = (snd_r & (SND_RING_FRAMES - 1)) * 2;
            out[2 * i] = snd_ring[p]; out[2 * i + 1] = snd_ring[p + 1];
            snd_r++; avail--;
        } else { out[2 * i] = out[2 * i + 1] = 0; }
    }
    b->mAudioDataByteSize = frames * 4;
    snd_enqueue(q, b, 0, NULL);
}

static int snd_init(int rate) {
    void *at = dlopen("/System/Library/Frameworks/AudioToolbox.framework/AudioToolbox", RTLD_NOW);
    if (!at) return -1;
    OSStatus (*NewOut)(const AudioStreamBasicDescription *, AudioQueueOutputCallback, void *, CFRunLoopRef, CFStringRef, UInt32, AudioQueueRef *) = dlsym(at, "AudioQueueNewOutput");
    OSStatus (*Alloc)(AudioQueueRef, UInt32, AudioQueueBufferRef *) = dlsym(at, "AudioQueueAllocateBuffer");
    OSStatus (*Start)(AudioQueueRef, const AudioTimeStamp *) = dlsym(at, "AudioQueueStart");
    snd_enqueue = dlsym(at, "AudioQueueEnqueueBuffer");
    snd_stop = dlsym(at, "AudioQueueStop");
    snd_dispose = dlsym(at, "AudioQueueDispose");
    if (!NewOut || !Alloc || !Start || !snd_enqueue || !snd_stop || !snd_dispose) return -1;

    AudioStreamBasicDescription f;
    memset(&f, 0, sizeof f);
    f.mSampleRate = rate; f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    f.mChannelsPerFrame = 2; f.mBitsPerChannel = 16;
    f.mBytesPerFrame = 4; f.mFramesPerPacket = 1; f.mBytesPerPacket = 4;
    if (NewOut(&f, snd_cb, NULL, NULL, NULL, 0, &snd_q)) { snd_q = NULL; return -1; }
    for (int i = 0; i < SND_BUFS; i++) {
        AudioQueueBufferRef b = NULL;
        if (Alloc(snd_q, SND_BUF_FRAMES * 4, &b) || !b) return -1;
        snd_cb(NULL, snd_q, b);
    }
    return Start(snd_q, NULL) ? -1 : 0;
}

/* samples = interleaved stereo s16, frames = number of stereo frames */
static void snd_push(const int16_t *samples, int frames) {
    if (!snd_q) return;
    for (int i = 0; i < frames; i++) {
        if (snd_w - snd_r >= SND_RING_FRAMES) return;          /* full: drop the rest */
        unsigned p = (snd_w & (SND_RING_FRAMES - 1)) * 2;
        snd_ring[p] = samples[2 * i]; snd_ring[p + 1] = samples[2 * i + 1];
        snd_w++;
    }
}

static void snd_close(void) {
    if (snd_q) { snd_stop(snd_q, true); snd_dispose(snd_q, true); snd_q = NULL; }
}
#elif defined(__ANDROID__)
/* Android: a Java helper (music/android/Pcm.java, started with app_process) plays raw stereo PCM from a pipe through AudioTrack.
   app_process works while the Android framework is stopped, because AudioFlinger keeps running. The pipe is made small and
   non-blocking, so a slow helper drops sound instead of making the game wait, and the delay stays short. */
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#define SND_JAR "/data/local/tmp/musichelper.jar"
#ifndef F_SETPIPE_SZ
#define F_SETPIPE_SZ 1031
#endif
static int snd_fd = -1; static pid_t snd_pid;

static int snd_init(int rate) {
    signal(SIGPIPE, SIG_IGN);
    if (access(SND_JAR, R_OK)) { fprintf(stderr, "no sound: missing %s\n", SND_JAR); return -1; }
    int to[2], from[2];
    if (pipe(to) || pipe(from)) return -1;
    snd_pid = fork();
    if (snd_pid < 0) return -1;
    if (snd_pid == 0) {
        dup2(to[0], 0); dup2(from[1], 1);
        close(to[0]); close(to[1]); close(from[0]); close(from[1]);
        if (!getenv("ANDROID_ROOT")) setenv("ANDROID_ROOT", "/system", 1);
        if (!getenv("ANDROID_DATA")) setenv("ANDROID_DATA", "/data", 1);
        if (!getenv("LD_LIBRARY_PATH")) setenv("LD_LIBRARY_PATH", "/system/lib", 1);
        char r[16], v[16]; snprintf(r, sizeof r, "%d", rate); snprintf(v, sizeof v, "%d", plat_volume_get());
        const char *ap = "/system/bin/app_process";
        if (access("/system/bin/app_process64", X_OK) == 0 && access("/system/lib64", F_OK) == 0) {
            ap = "/system/bin/app_process64"; setenv("LD_LIBRARY_PATH", "/system/lib64", 1);
        }
        execl(ap, "app_process", "-Djava.class.path=" SND_JAR, "/system/bin", "Pcm", r, v, (char *)NULL);
        _exit(127);
    }
    close(to[0]); close(from[1]);
    fcntl(from[0], F_SETFL, fcntl(from[0], F_GETFL) | O_NONBLOCK);
    char buf[64]; int len = 0, ok = 0;
    for (int i = 0; i < 160 && !ok; i++) {                       /* wait up to 8 s for "ready" */
        ssize_t n = read(from[0], buf + len, sizeof buf - 1 - (size_t)len);
        if (n > 0) { len += (int)n; buf[len] = 0; if (strstr(buf, "ready")) ok = 1; }
        int st; if (waitpid(snd_pid, &st, WNOHANG) == snd_pid) { fprintf(stderr, "sound helper exited early\n"); close(to[1]); close(from[0]); snd_pid = 0; return -1; }
        if (!ok) usleep(50000);
    }
    close(from[0]);
    if (!ok) { fprintf(stderr, "sound helper did not start\n"); close(to[1]); kill(snd_pid, SIGTERM); snd_pid = 0; return -1; }
    fcntl(to[1], F_SETPIPE_SZ, 8192);                            /* about 46 ms of sound: the most that can wait in the pipe */
    fcntl(to[1], F_SETFL, fcntl(to[1], F_GETFL) | O_NONBLOCK);
    snd_fd = to[1];
    return 0;
}
/* samples = interleaved stereo s16, frames = number of stereo frames */
static void snd_push(const int16_t *samples, int frames) {
    if (snd_fd < 0) return;
    (void)!write(snd_fd, samples, (size_t)frames * 4);           /* a full pipe drops the sound */
}
static void snd_close(void) {
    if (snd_fd >= 0) { close(snd_fd); snd_fd = -1; }
    if (snd_pid > 0) { usleep(200000); kill(snd_pid, SIGTERM); waitpid(snd_pid, NULL, 0); snd_pid = 0; }
}
#else
static int snd_init(int rate) { (void)rate; return -1; }
static void snd_push(const int16_t *s, int n) { (void)s; (void)n; }
static void snd_close(void) {}
#endif

#endif
