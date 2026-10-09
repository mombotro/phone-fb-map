// decode.c - FLAC (dr_flac), Ogg Vorbis (stb_vorbis), and everything Apple can decode (ExtAudioFile:
// MP3, M4A/AAC, WAV, AIFF, CAF). AudioToolbox loads at run time, so no framework link is needed.
#include "decode.h"
#include <AudioToolbox/AudioToolbox.h>
#include <ctype.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include "third_party/dr_flac.h"

#define STB_VORBIS_HEADER_ONLY
#include "third_party/stb_vorbis.c"

enum { K_EXT, K_FLAC, K_OGG };

struct Dec {
    int kind, rate;
    long total;
    drflac *flac;
    stb_vorbis *ogg;
    ExtAudioFileRef ext;
    int16_t *tmp;               // channel-conversion scratch for FLAC
    long tmp_frames;
    int channels;
    int err;                    // last error from the decoder, for logging
};

static struct {
    OSStatus (*Open)(CFURLRef, ExtAudioFileRef *);
    OSStatus (*Get)(ExtAudioFileRef, ExtAudioFilePropertyID, UInt32 *, void *);
    OSStatus (*Set)(ExtAudioFileRef, ExtAudioFilePropertyID, UInt32, const void *);
    OSStatus (*Read)(ExtAudioFileRef, UInt32 *, AudioBufferList *);
    OSStatus (*Seek)(ExtAudioFileRef, SInt64);
    OSStatus (*Dispose)(ExtAudioFileRef);
    int loaded;
} ea;

static int ext_load(void) {
    if (ea.loaded) return ea.Open != NULL;
    ea.loaded = 1;
    void *at = dlopen("/System/Library/Frameworks/AudioToolbox.framework/AudioToolbox", RTLD_NOW);
    if (!at) return 0;
    ea.Open = dlsym(at, "ExtAudioFileOpenURL");
    ea.Get = dlsym(at, "ExtAudioFileGetProperty");
    ea.Set = dlsym(at, "ExtAudioFileSetProperty");
    ea.Read = dlsym(at, "ExtAudioFileRead");
    ea.Seek = dlsym(at, "ExtAudioFileSeek");
    ea.Dispose = dlsym(at, "ExtAudioFileDispose");
    if (!ea.Open || !ea.Get || !ea.Set || !ea.Read || !ea.Seek || !ea.Dispose) ea.Open = NULL;
    return ea.Open != NULL;
}

static int has_ext(const char *name, const char *e) {
    size_t n = strlen(name), m = strlen(e);
    if (n <= m) return 0;
    for (size_t i = 0; i < m; i++) if (tolower((unsigned char)name[n - m + i]) != e[i]) return 0;
    return 1;
}

static int kind_of(const char *name) {
    if (has_ext(name, ".flac")) return K_FLAC;
    if (has_ext(name, ".ogg")) return K_OGG;
    static const char *apple[] = { ".mp3", ".m4a", ".aac", ".wav", ".aif", ".aiff", ".caf", ".m4b", ".mp4" };
    for (unsigned i = 0; i < sizeof apple / sizeof apple[0]; i++) if (has_ext(name, apple[i])) return K_EXT;
    return -1;
}

int dec_supported(const char *name) { return kind_of(name) >= 0; }

Dec *dec_open(const char *path) {
    int kind = kind_of(path);
    if (kind < 0) return NULL;
    Dec *d = calloc(1, sizeof *d);
    d->kind = kind; d->total = -1;

    if (kind == K_FLAC) {
        d->flac = drflac_open_file(path, NULL);
        if (!d->flac) { free(d); return NULL; }
        d->rate = (int)d->flac->sampleRate;
        d->channels = d->flac->channels;
        d->total = (long)d->flac->totalPCMFrameCount;
    } else if (kind == K_OGG) {
        int err = 0;
        d->ogg = stb_vorbis_open_filename(path, &err, NULL);
        if (!d->ogg) { free(d); return NULL; }
        stb_vorbis_info info = stb_vorbis_get_info(d->ogg);
        d->rate = (int)info.sample_rate;
        d->channels = info.channels;
        d->total = (long)stb_vorbis_stream_length_in_samples(d->ogg);
    } else {
        if (!ext_load()) { free(d); return NULL; }
        CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, (CFIndex)strlen(path), false);
        OSStatus rc = url ? ea.Open(url, &d->ext) : -1;
        if (url) CFRelease(url);
        if (rc) { free(d); return NULL; }
        AudioStreamBasicDescription ff; UInt32 sz = sizeof ff;
        if (ea.Get(d->ext, kExtAudioFileProperty_FileDataFormat, &sz, &ff) || ff.mSampleRate < 1000) { ea.Dispose(d->ext); free(d); return NULL; }
        d->rate = (int)ff.mSampleRate;
        AudioStreamBasicDescription cf;
        memset(&cf, 0, sizeof cf);
        cf.mSampleRate = ff.mSampleRate; cf.mFormatID = kAudioFormatLinearPCM;
        cf.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
        cf.mChannelsPerFrame = 2; cf.mBitsPerChannel = 16;
        cf.mBytesPerFrame = 4; cf.mFramesPerPacket = 1; cf.mBytesPerPacket = 4;
        if (ea.Set(d->ext, kExtAudioFileProperty_ClientDataFormat, sizeof cf, &cf)) { ea.Dispose(d->ext); free(d); return NULL; }
        SInt64 len = 0; sz = sizeof len;
        if (!ea.Get(d->ext, kExtAudioFileProperty_FileLengthFrames, &sz, &len)) d->total = (long)len;
    }
    return d;
}

int dec_rate(const Dec *d) { return d->rate; }
long dec_total_frames(const Dec *d) { return d->total; }

long dec_read(Dec *d, int16_t *out, long frames) {
    if (d->kind == K_OGG) {
        int n = stb_vorbis_get_samples_short_interleaved(d->ogg, 2, out, (int)(frames * 2));
        return n;
    }
    if (d->kind == K_FLAC) {
        if (d->channels == 2) return (long)drflac_read_pcm_frames_s16(d->flac, (drflac_uint64)frames, out);
        if (d->tmp_frames < frames) {
            free(d->tmp);
            d->tmp = malloc((size_t)frames * d->channels * sizeof(int16_t));
            d->tmp_frames = frames;
        }
        long n = (long)drflac_read_pcm_frames_s16(d->flac, (drflac_uint64)frames, d->tmp);
        for (long i = 0; i < n; i++) {                       // mono: copy to both sides; more than 2: first two channels
            int16_t l = d->tmp[i * d->channels];
            int16_t r = d->channels > 1 ? d->tmp[i * d->channels + 1] : l;
            out[2 * i] = l; out[2 * i + 1] = r;
        }
        return n;
    }
    UInt32 n = (UInt32)frames;
    AudioBufferList abl;
    abl.mNumberBuffers = 1;
    abl.mBuffers[0].mNumberChannels = 2;
    abl.mBuffers[0].mDataByteSize = (UInt32)(frames * 4);
    abl.mBuffers[0].mData = out;
    OSStatus rc = ea.Read(d->ext, &n, &abl);
    if (rc) { d->err = (int)rc; return 0; }
    return (long)n;
}

int dec_seek(Dec *d, long frame) {
    if (frame < 0) frame = 0;
    if (d->kind == K_OGG) return stb_vorbis_seek(d->ogg, (unsigned)frame) ? 0 : -1;
    if (d->kind == K_FLAC) return drflac_seek_to_pcm_frame(d->flac, (drflac_uint64)frame) ? 0 : -1;
    return ea.Seek(d->ext, frame) ? -1 : 0;
}

int dec_error(const Dec *d) { return d->err; }

void dec_close(Dec *d) {
    if (!d) return;
    if (d->flac) drflac_close(d->flac);
    if (d->ogg) stb_vorbis_close(d->ogg);
    if (d->ext) ea.Dispose(d->ext);
    free(d->tmp);
    free(d);
}
