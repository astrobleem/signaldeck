/* Unsigned 8-bit mono PCM WAV playback through ELKS /dev/dsp. */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <sys/audio.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <unistd.h>
#include "sbpcm.h"

#define PCM_MIN_RATE 4000UL
#define PCM_MAX_RATE 20000UL
#define PCM_MAX_SECONDS 240UL
#define PCM_MAX_HEADER 65536UL
#define PCM_MAX_CHUNKS 128U
#define PCM_BUFFER 4096
static unsigned char pcm[PCM_BUFFER];
static volatile unsigned char interrupted;

struct wave_info {
    unsigned long rate;
    unsigned long offset;
    unsigned long samples;
};

static void stop_playback(int sig)
{
    (void)sig;
    interrupted = 1;
}

static unsigned int le16(const unsigned char *p)
{
    return p[0] | ((unsigned int)p[1] << 8);
}

static unsigned long le32(const unsigned char *p)
{
    return (unsigned long)le16(p) | ((unsigned long)le16(p+2) << 16);
}

static int read_all(int fd, unsigned char *p, unsigned int n)
{
    int got;
    while (n && !interrupted) {
        got = read(fd, p, n);
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0)
            return -1;
        n -= got;
        p += got;
    }
    return n ? -1 : 0;
}

static int write_all(int fd, const unsigned char *p, unsigned int n)
{
    int got;
    while (n && !interrupted) {
        got = write(fd, p, n);
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0)
            return -1;
        n -= got;
        p += got;
    }
    return n ? -1 : 0;
}

static int parse_wave(int fd, struct wave_info *out)
{
    unsigned char b[18];
    unsigned long p, end, size, remaining;
    long actual = lseek(fd, 0L, SEEK_END);
    unsigned int count = 0;
    char have_fmt = 0, have_data = 0;

    if (actual < 12L || (unsigned long)actual >
        PCM_MAX_HEADER + PCM_MAX_RATE * PCM_MAX_SECONDS ||
        lseek(fd, 0L, SEEK_SET) != 0L || read_all(fd, b, 12) ||
        memcmp(b, "RIFF", 4) || memcmp(b+8, "WAVE", 4) ||
        le32(b+4) != (unsigned long)actual - 8UL)
        return -1;

    end = (unsigned long)actual;
    for (p = 12; p < end; ) {
        if (++count > PCM_MAX_CHUNKS || end - p < 8 ||
            lseek(fd, (long)p, SEEK_SET) != (long)p || read_all(fd, b, 8))
            return -1;
        size = le32(b+4);
        p += 8;
        remaining = end - p;
        if (size > remaining || ((size & 1UL) && size == remaining))
            return -1;
        if (!memcmp(b, "fmt ", 4)) {
            if (have_fmt || (size != 16 && size != 18) ||
                read_all(fd, b, (unsigned int)size) ||
                le16(b) != 1 || le16(b+2) != 1 || le16(b+12) != 1 ||
                le16(b+14) != 8 || (size == 18 && le16(b+16)))
                return -1;
            out->rate = le32(b+4);
            if (out->rate < PCM_MIN_RATE || out->rate > PCM_MAX_RATE ||
                le32(b+8) != out->rate)
                return -1;
            have_fmt = 1;
        } else if (!memcmp(b, "data", 4)) {
            if (!have_fmt || have_data || !size ||
                size > out->rate * PCM_MAX_SECONDS)
                return -1;
            out->offset = p;
            out->samples = size;
            have_data = 1;
        }
        p += size + (size & 1UL);
    }
    return have_data && lseek(fd, (long)out->offset, SEEK_SET) ==
                        (long)out->offset ? 0 : -1;
}

const char *sbpcm_result_text(enum sbpcm_result result)
{
    switch (result) {
    case SBPCM_OK: return "Playback complete";
    case SBPCM_CANCELED: return "Playback stopped";
    case SBPCM_FILE: return "Cannot open WAV";
    case SBPCM_FORMAT: return "Expected PCM WAV: 8-bit mono, 4-20 kHz, at most 240 s";
    case SBPCM_DEVICE: return "Cannot open /dev/dsp";
    case SBPCM_CONFIG: return "Sound Blaster PCM setup failed";
    default: return "Sound Blaster PCM read/write/drain or cleanup failed";
    }
}

enum sbpcm_result sbpcm_play_file(const char *path)
{
    enum sbpcm_result result = SBPCM_FILE;
    struct wave_info w = {0, 0, 0};
    struct audio_errinfo stats;
    unsigned int n, chunk = PCM_BUFFER;
    int in = -1, dsp = -1;
    int32_t v;
    void (*oldint)(int), (*oldterm)(int);

    interrupted = 0;
    oldint = signal(SIGINT, stop_playback);
    if (oldint == SIG_ERR)
        return SBPCM_IO;
    oldterm = signal(SIGTERM, stop_playback);
    if (oldterm == SIG_ERR) {
        signal(SIGINT, oldint);
        return SBPCM_IO;
    }

    in = open(path, O_RDONLY);
    if (in < 0)
        goto done;
    result = SBPCM_FORMAT;
    if (parse_wave(in, &w) || interrupted)
        goto done;

    result = SBPCM_DEVICE;
    dsp = open("/dev/dsp", O_WRONLY);
    if (dsp < 0)
        goto done;
    result = SBPCM_CONFIG;
    v = DSP_FMT_U8;
    if (ioctl(dsp, SNDCTL_DSP_SETFMT, &v) < 0 || v != DSP_FMT_U8)
        goto done;
    v = 1;
    if (ioctl(dsp, SNDCTL_DSP_CHANNELS, &v) < 0 || v != 1)
        goto done;
    v = (int32_t)w.rate;
    if (ioctl(dsp, SNDCTL_DSP_SPEED, &v) < 0 ||
        (unsigned long)v < PCM_MIN_RATE || (unsigned long)v > PCM_MAX_RATE)
        goto done;
    v = 0;
    if (ioctl(dsp, SNDCTL_DSP_GETBLKSIZE, &v) == 0 && v > 0 && v < PCM_BUFFER)
        chunk = (unsigned int)v;

    result = SBPCM_IO;
    while (w.samples && !interrupted) {
        n = w.samples > chunk ? chunk : (unsigned int)w.samples;
        if (read_all(in, pcm, n) || write_all(dsp, pcm, n))
            goto done;
        w.samples -= n;
    }
    if (interrupted || ioctl(dsp, SNDCTL_DSP_SYNC, 0) < 0)
        goto done;
    if (ioctl(dsp, SNDCTL_DSP_GETERROR, &stats) == 0 && stats.play_underruns)
        goto done;
    result = SBPCM_OK;

done:
    if (interrupted)
        result = SBPCM_CANCELED;
    if (dsp >= 0) {
        if (result != SBPCM_OK && ioctl(dsp, SNDCTL_DSP_RESET, 0) < 0)
            result = SBPCM_IO;
        if (close(dsp) < 0)
            result = SBPCM_IO;
    }
    if (in >= 0)
        close(in);
    signal(SIGTERM, oldterm);
    signal(SIGINT, oldint);
    return result;
}
