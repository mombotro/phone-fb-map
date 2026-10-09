// decode.h - one decoder interface for every format the music player plays.
// Output is always 16-bit stereo interleaved, at the file's own sample rate.
#ifndef DECODE_H
#define DECODE_H

#include <stdint.h>

typedef struct Dec Dec;

int   dec_supported(const char *name);                    // by file extension
Dec  *dec_open(const char *path);                         // NULL on error
int   dec_rate(const Dec *d);
long  dec_total_frames(const Dec *d);                     // -1 if unknown
long  dec_read(Dec *d, int16_t *out, long frames);        // frames read; 0 = end of file or error
int   dec_seek(Dec *d, long frame);                       // 0 on success
int   dec_error(const Dec *d);                            // last decoder error code (0 = none)
void  dec_close(Dec *d);

#endif
