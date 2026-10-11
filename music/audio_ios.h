// audio_ios.h - audio engine of the iOS music player: decoders (decode.h) feeding an AudioQueue.
// Included by player.c, which defines g_volume, now() and LOG first.
#ifndef AUDIO_IOS_H
#define AUDIO_IOS_H
#include "decode.h"
#include <AudioToolbox/AudioToolbox.h>
#include <dlfcn.h>
#define BUF_FRAMES 4096
#define N_BUFS 3


static struct {
    OSStatus (*NewOutput)(const AudioStreamBasicDescription *, AudioQueueOutputCallback, void *, CFRunLoopRef, CFStringRef, UInt32, AudioQueueRef *);
    OSStatus (*Alloc)(AudioQueueRef, UInt32, AudioQueueBufferRef *);
    OSStatus (*Enqueue)(AudioQueueRef, AudioQueueBufferRef, UInt32, const AudioStreamPacketDescription *);
    OSStatus (*Start)(AudioQueueRef, const AudioTimeStamp *);
    OSStatus (*Pause)(AudioQueueRef);
    OSStatus (*Stop)(AudioQueueRef, Boolean);
    OSStatus (*Dispose)(AudioQueueRef, Boolean);
    OSStatus (*SetParam)(AudioQueueRef, AudioQueueParameterID, AudioQueueParameterValue);
} aq;

static Dec *g_dec;
static AudioQueueRef g_queue;
static volatile int g_eof;                // decoder reached the end of the file
static volatile long g_decoded;           // frames decoded so far (the audio thread updates it)
static volatile long g_seek = -1;         // frame to seek to (the audio thread applies it)
static int g_paused, g_rate, g_playing;
static long g_total;

static void fill_cb(void *user, AudioQueueRef q, AudioQueueBufferRef b) {
    (void)user;
    int16_t *out = b->mAudioData;
    long frames = b->mAudioDataBytesCapacity / 4;
    if (g_seek >= 0 && g_dec) { dec_seek(g_dec, g_seek); g_decoded = g_seek; g_seek = -1; g_eof = 0; }
    long n = (g_eof || !g_dec) ? 0 : dec_read(g_dec, out, frames);
    if (n < 0) n = 0;
    if (n < frames) { memset(out + 2 * n, 0, (size_t)(frames - n) * 4); if (n == 0) g_eof = 1; }
    g_decoded += n;
    b->mAudioDataByteSize = (UInt32)(frames * 4);
    aq.Enqueue(q, b, 0, NULL);
}

static int audio_load(void) {
    void *at = dlopen("/System/Library/Frameworks/AudioToolbox.framework/AudioToolbox", RTLD_NOW);
    if (!at) return -1;
    aq.NewOutput = dlsym(at, "AudioQueueNewOutput");
    aq.Alloc = dlsym(at, "AudioQueueAllocateBuffer");
    aq.Enqueue = dlsym(at, "AudioQueueEnqueueBuffer");
    aq.Start = dlsym(at, "AudioQueueStart");
    aq.Pause = dlsym(at, "AudioQueuePause");
    aq.Stop = dlsym(at, "AudioQueueStop");
    aq.Dispose = dlsym(at, "AudioQueueDispose");
    aq.SetParam = dlsym(at, "AudioQueueSetParameter");
    return (aq.NewOutput && aq.Alloc && aq.Enqueue && aq.Start && aq.Pause && aq.Stop && aq.Dispose && aq.SetParam) ? 0 : -1;
}

static void stop_audio(void) {
    if (g_queue) { aq.Stop(g_queue, true); aq.Dispose(g_queue, true); g_queue = NULL; }
    if (g_dec) { dec_close(g_dec); g_dec = NULL; }
    g_playing = 0; g_paused = 0; g_eof = 0; g_decoded = 0; g_seek = -1;
}

// Open a file and start playing it. Returns 0 on success.
static int play_file(const char *path) {
    stop_audio();
    g_dec = dec_open(path);
    if (!g_dec) return -1;
    g_rate = dec_rate(g_dec);
    g_total = dec_total_frames(g_dec);
    LOG("play %s: %d Hz, %ld frames (%.1f s)", path, g_rate, g_total, g_rate ? (double)g_total / g_rate : 0.0);

    AudioStreamBasicDescription fmt;
    memset(&fmt, 0, sizeof fmt);
    fmt.mSampleRate = g_rate; fmt.mFormatID = kAudioFormatLinearPCM;
    fmt.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    fmt.mChannelsPerFrame = 2; fmt.mBitsPerChannel = 16;
    fmt.mBytesPerFrame = 4; fmt.mFramesPerPacket = 1; fmt.mBytesPerPacket = 4;
    if (aq.NewOutput(&fmt, fill_cb, NULL, NULL, NULL, 0, &g_queue)) { stop_audio(); return -1; }
    aq.SetParam(g_queue, kAudioQueueParam_Volume, g_volume);
    for (int i = 0; i < N_BUFS; i++) {
        AudioQueueBufferRef b = NULL;
        if (aq.Alloc(g_queue, BUF_FRAMES * 4, &b) || !b) { stop_audio(); return -1; }
        fill_cb(NULL, g_queue, b);
    }
    if (aq.Start(g_queue, NULL)) { stop_audio(); return -1; }
    g_playing = 1;
    return 0;
}

static double position_seconds(void) {
    long p = g_decoded - (long)N_BUFS * BUF_FRAMES;           // audio still queued has not played yet
    if (p < 0) p = 0;
    return g_rate > 0 ? (double)p / g_rate : 0;
}


// ---- the interface the player UI uses (the Android backend has the same functions) ----
static double audio_duration(void) { return (g_total > 0 && g_rate > 0) ? (double)g_total / g_rate : 0; }
static double audio_fraction(void) {
    if (g_total <= 0) return 0;
    if (g_seek >= 0) return (double)g_seek / g_total;
    double f = (double)(g_decoded - (long)N_BUFS * BUF_FRAMES) / g_total;
    return f < 0 ? 0 : (f > 1 ? 1 : f);
}
static void audio_seek_fraction(double f) { if (g_total > 0) g_seek = (long)(f * g_total); }
static void audio_set_paused(int p) {
    if (!g_queue) return;
    if (p) aq.Pause(g_queue); else aq.Start(g_queue, NULL);
    g_paused = p;
}
static void audio_set_volume(float v) { if (g_queue) aq.SetParam(g_queue, kAudioQueueParam_Volume, v); }
static void audio_poll(void) {}
static void audio_end_info(char *b, size_t n) { snprintf(b, n, "decoded %ld of %ld frames, decoder error %d", (long)g_decoded, g_total, g_dec ? dec_error(g_dec) : 0); }
static void audio_info(char *b, size_t n) { snprintf(b, n, "decoded=%ld eof=%d", (long)g_decoded, g_eof); }

#endif
