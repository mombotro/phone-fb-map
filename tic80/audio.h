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
#else
static int snd_init(int rate) { (void)rate; return -1; }
static void snd_push(const int16_t *s, int n) { (void)s; (void)n; }
static void snd_close(void) {}
#endif

#endif
