// tone.c - plays a quiet 440 Hz tone with AudioQueue to prove audio output works from a root command-line tool.
// usage: tone [session]     ("session" = activate an AudioSession first)
// AudioToolbox loads at run time, so no framework link is needed.
#include <AudioToolbox/AudioToolbox.h>
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static double g_phase;
static int g_filled;

static void fill_cb(void *user, AudioQueueRef q, AudioQueueBufferRef buf) {
    (void)user;
    int frames = buf->mAudioDataBytesCapacity / 4;
    int16_t *out = buf->mAudioData;
    for (int i = 0; i < frames; i++) {
        int16_t v = (int16_t)(sin(g_phase) * 4500);          // about 14% of full scale
        g_phase += 2 * M_PI * 440.0 / 44100.0;
        out[2 * i] = out[2 * i + 1] = v;
    }
    buf->mAudioDataByteSize = frames * 4;
    g_filled++;
    void *at = dlopen("/System/Library/Frameworks/AudioToolbox.framework/AudioToolbox", RTLD_NOW);
    OSStatus (*Enqueue)(AudioQueueRef, AudioQueueBufferRef, UInt32, const AudioStreamPacketDescription *) = dlsym(at, "AudioQueueEnqueueBuffer");
    Enqueue(q, buf, 0, NULL);
}

int main(int argc, char **argv) {
    void *at = dlopen("/System/Library/Frameworks/AudioToolbox.framework/AudioToolbox", RTLD_NOW);
    if (!at) { printf("dlopen AudioToolbox: %s\n", dlerror()); return 1; }

    if (argc > 1 && !strcmp(argv[1], "session")) {
        OSStatus (*SInit)(CFRunLoopRef, CFStringRef, void *, void *) = dlsym(at, "AudioSessionInitialize");
        OSStatus (*SSet)(UInt32, UInt32, const void *) = dlsym(at, "AudioSessionSetProperty");
        OSStatus (*SAct)(Boolean) = dlsym(at, "AudioSessionSetActive");
        printf("session symbols: %p %p %p\n", SInit, SSet, SAct);
        if (SInit && SSet && SAct) {
            printf("AudioSessionInitialize -> %d\n", (int)SInit(NULL, NULL, NULL, NULL));
            UInt32 cat = 'mdia';                              // kAudioSessionCategory_MediaPlayback
            printf("SetProperty category -> %d\n", (int)SSet('ocat', sizeof cat, &cat));   // kAudioSessionProperty_AudioCategory
            printf("SetActive -> %d\n", (int)SAct(true));
        }
    }

    OSStatus (*NewOut)(const AudioStreamBasicDescription *, AudioQueueOutputCallback, void *, CFRunLoopRef, CFStringRef, UInt32, AudioQueueRef *) = dlsym(at, "AudioQueueNewOutput");
    OSStatus (*Alloc)(AudioQueueRef, UInt32, AudioQueueBufferRef *) = dlsym(at, "AudioQueueAllocateBuffer");
    OSStatus (*Start)(AudioQueueRef, const AudioTimeStamp *) = dlsym(at, "AudioQueueStart");
    OSStatus (*Stop)(AudioQueueRef, Boolean) = dlsym(at, "AudioQueueStop");
    OSStatus (*Dispose)(AudioQueueRef, Boolean) = dlsym(at, "AudioQueueDispose");
    printf("queue symbols: %p %p %p %p %p\n", NewOut, Alloc, Start, Stop, Dispose);
    if (!NewOut || !Alloc || !Start) return 1;

    AudioStreamBasicDescription fmt;
    memset(&fmt, 0, sizeof fmt);
    fmt.mSampleRate = 44100; fmt.mFormatID = kAudioFormatLinearPCM;
    fmt.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    fmt.mChannelsPerFrame = 2; fmt.mBitsPerChannel = 16;
    fmt.mBytesPerFrame = 4; fmt.mFramesPerPacket = 1; fmt.mBytesPerPacket = 4;

    AudioQueueRef q = NULL;
    OSStatus rc = NewOut(&fmt, fill_cb, NULL, NULL, NULL, 0, &q);
    printf("AudioQueueNewOutput -> %d queue=%p\n", (int)rc, q);
    if (rc) return 1;

    for (int i = 0; i < 3; i++) {
        AudioQueueBufferRef b = NULL;
        rc = Alloc(q, 4096 * 4, &b);
        printf("AllocateBuffer %d -> %d\n", i, (int)rc);
        if (rc || !b) return 1;
        fill_cb(NULL, q, b);
    }
    rc = Start(q, NULL);
    printf("AudioQueueStart -> %d  (listen for a beep)\n", (int)rc);
    fflush(stdout);
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 1.2, false);
    printf("buffers filled: %d\n", g_filled);
    Stop(q, true);
    Dispose(q, true);
    printf("done\n");
    return 0;
}
