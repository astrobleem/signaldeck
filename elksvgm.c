/* Signal Deck: native ELKS console player.
 * PC-9801-26/K and Tandy EX streams validated before hardware I/O.
 * Optional 26/K SSG writes audio registers only, with mixer readback
 * and unrelated voices/GPIO directions preserved. No IRQ, timer,
 * prescaler, envelope, data blocks, or loop replay. */

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <sys/utsname.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <termios.h>

#define FM_ADDR 0x188
#define FM_DATA 0x18a
#define MAX_FILE 8192U
#define MAX_SAMPLES (120UL * 44100UL)

static unsigned char image[MAX_FILE + 1];
static volatile unsigned char stopped, signal_stop;
static unsigned char touched, psg, pcm, opl, ssg, pcstream, tstream;
#define STREAMING (pcstream || tstream)
static unsigned char stream_ready_next;
static char *cue_files[99];
static unsigned char ssg_claim, ssg_touched, noise_claim;
static unsigned char mixer_start, mixer_state, mixer_mask, mixer_ok;
static unsigned char noise_start, noise_changed;
static unsigned char ssg_volume[3];
static const char *reject;
static int open_failure;
static struct timeval start;
static unsigned int loop_pos, first, end;

#include "ui.inc"
#include "status.h"

static void on_signal(int sig)
{
    (void)sig;
    signal_stop = stopped = 1;
}

static unsigned long le32(unsigned int p)
{
    return (unsigned long)image[p] |
           ((unsigned long)image[p+1] << 8) |
           ((unsigned long)image[p+2] << 16) |
           ((unsigned long)image[p+3] << 24);
}

static int ready(void)
{
    unsigned char s;
    unsigned int i = 4096;
    do {
        asm volatile ("inb %w1,%0" : "=a" (s) : "d" (FM_ADDR));
        if (s == 0xff)
            return -1;
        if (!(s & 0x80))
            return 0;
    } while (--i);
    return -1;
}

static int put(unsigned char r, unsigned char v)
{
    if (ready())
        return -1;
    asm volatile ("outb %0,%w1" :: "a" (r), "d" (FM_ADDR));
    if (ready())
        return -1;
    asm volatile ("outb %0,%w1" :: "a" (v), "d" (FM_DATA));
    return 0;
}

static int real_mode(void)
{
    unsigned int f;
    asm volatile ("pushf; popw %0" : "=a" (f));
    if ((f & 0xf000) == 0xf000)
        return 1;                 /* 8086/186 flags, no SMSW instruction */
    asm volatile (".byte 0x0f,0x01,0xe0" : "=a" (f)); /* SMSW AX on 286+ */
    return !(f & 1);
}

static int tandy_rom(void)
{
    const unsigned char __far *rom = (const unsigned char __far *)0xf0000000UL;
    /* Family fingerprint; the explicit switch still requires a known EX. */
    return rom[0xc000U] == 0x21 && rom[0xfffeU] == 0xff;
}

static void psg_write(unsigned char v)
{
    /* SN76496 boards need time between back-to-back OUTs. */
    asm volatile ("outb %0,$0xc0; nop; nop; nop; nop; nop; nop;"
                  "nop; nop; nop; nop; nop; nop" :: "a" (v));
}

static int ssg_read(unsigned char r, unsigned char *v)
{
    if (ready())
        return -1;
    asm volatile ("outb %0,%w1" :: "a" (r), "d" (FM_ADDR));
    if (ready())
        return -1;
    asm volatile ("inb %w1,%0" : "=a" (*v) : "d" (FM_DATA));
    return 0;
}

static int ssg_probe(void)
{
    unsigned char v, a, b, op, ch;
    if (ssg_read(7, &mixer_start) || ssg_read(7, &v) || v != mixer_start)
        return -1;
    mixer_state = mixer_start;
    mixer_mask = ssg_claim | (ssg_claim << 3);
    if (noise_claim) {
        /* Preserve shared noise for any unrelated enabled voice. */
        if (((mixer_start >> 3) & ~ssg_claim & 7) != (~ssg_claim & 7) ||
            ssg_read(6, &noise_start) || noise_start > 31)
            return -1;
    }
    for (ch = 0; ch < 3; ++ch)
        if (ssg_claim & (1 << ch)) {
            if (ssg_read(8 + ch, &v) || v > 31)
                return -1;
            ssg_touched |= 1 << ch;
            if (put(8 + ch, 0) || ssg_read(8 + ch, &v) || v)
                return -1;
        }
    for (ch = 0; !(ssg_claim & (1 << ch)); ++ch)
        ;
    op = ch << 1;
    /* Toggle and restore a harmless tone period while claimed voices
     * are silent, refusing a constant/floating or misrouted data bus. */
    if (ssg_read(op, &a) || put(op, a ^ 0x55))
        return -1;
    if (ssg_read(op, &b) || b != (a ^ 0x55)) {
        put(op, a);
        return -1;
    }
    if (put(op, a) || ssg_read(op, &b) || b != a ||
        ssg_read(7, &v) || v != mixer_state)
        return -1;
    mixer_ok = 1;
    return 0;
}

static int mixer_put(unsigned char bits)
{
    unsigned char old, now;
    if (!mixer_ok || ssg_read(7, &old) || old != mixer_state)
        goto bad;
    now = (old & ~mixer_mask) | (bits & mixer_mask);
    if (old != now && (put(7, now) || ssg_read(7, &old) || old != now))
        goto bad;
    mixer_state = now;
    return 0;
bad:
    mixer_ok = 0;
    return -1;
}

static int ssg_reg(unsigned char r, unsigned char v, int *ch)
{
    *ch = -1;
    if (r < 6) {
        *ch = r >> 1;
        return !(r & 1) || v < 16;
    }
    if (r == 6)
        return v < 32;
    if (r == 7)
        return 1; /* GPIO bits in the file are ignored, never applied. */
    if (r >= 8 && r <= 10) {
        *ch = r - 8;
        return v < 16; /* fixed volume; no shared envelope generator */
    }
    return 0;
}

static void ssg_meter(void)
{
    unsigned char ch;
    if (!ui)
        return;
    for (ch = 0; ch < 3; ++ch) {
        if (ssg_touched & (1 << ch))
            meter_known |= 1 << (ch + 3);
        meter_level(ch + 3, (!(mixer_state & (1 << ch)) ||
                             !(mixer_state & (8 << ch))) ? ssg_volume[ch] : 0);
    }
}

static int fm_reg(unsigned char r, unsigned char v)
{
    if (r == 0x28)
        return !(v & 0x0c) && (v & 3) != 3;
    if ((r & 3) == 3)
        return 0;
    return (r >= 0x30 && r <= 0x9e) ||
           (r >= 0xa0 && r <= 0xa6) ||
           (r >= 0xb0 && r <= 0xb2);
}

/* Absolute schedule: round to the first available ELKS tick, not per wait.
 * Chunking also keeps surplus-sleep and signal response bounded.
 */
static int until(unsigned long samples)
{
    struct timeval now;
    long secs, elapsed, deadline, pause;
    unsigned long pos;
    int pumped = 0;
    deadline = (long)((samples / 441) * 10000UL +
                      ((samples % 441) * 10000UL + 440UL) / 441);
    while (!stopped) {
        if (gettimeofday(&now, 0))
            return -1;
        secs = now.tv_sec - start.tv_sec;
        if (secs < 0 || secs > 122)
            return -1;
        elapsed = secs * 1000000L + now.tv_usec - start.tv_usec;
        if (elapsed < 0)
            return -1;
        if (elapsed >= deadline)
            return 0;          /* a due write always goes before the display */
        pause = deadline - elapsed;
        /* Keys and meters only use slack of at least one ELKS tick, once
         * per sleep, then the clock is read again before sleeping. */
        if (ui && !pumped && pause >= 10000L) {
            pumped = 1;
            pos = ((unsigned long)elapsed / 1000UL * 441UL) / 10UL;
            if (deck_pump(pos))
                return -1;
            continue;
        }
        if (pause > (ui ? 20000L : 100000L))
            pause = ui ? 20000L : 100000L;
        usleep(pause);
        pumped = 0;
    }
    return -1;
}

static int walk(unsigned int p, unsigned int end, int play)
{
    unsigned long samples = 0;
    unsigned int wait;
    unsigned char cmd, r, v;
    int writes = 0, latched = -1, ssg_mix = 0;
    int loop_seen = !loop_pos;

    reject = "Malformed VGM stream";

    while (p < end && !stopped) {
        if (p == loop_pos)
            loop_seen = 1;
        cmd = image[p++];
        if (cmd == 0x66) {
            if (!play)
                total = samples;
            if ((ssg_mix || noise_claim) && !ssg_claim) {
                reject = "SSG mixer/noise needs a voice";
                return -1;
            }
            return (p == end && writes && samples && loop_seen) ? 0 : -1;
        }
        if (cmd == 0x50 && psg) {
            if (p >= end)
                return -1;
            v = image[p++];
            if (v & 0x80)
                latched = (v >> 4) & 7;
            else if ((v & 0x40) || latched < 0 ||
                     (latched & 1) || latched == 6)
                return -1;
            ++writes;
            if (play) {
                if (until(samples))
                    return -1;
                touched = 1;
                psg_write(v);
                if (ui && (latched & 1)) {
                    meter_level(latched >> 1, 15 - (v & 15));
                    meter_known |= 1 << (latched >> 1);
                }
            }
            continue;
        }
        if (cmd == 0x55 && !psg) {
            if (end - p < 2)
                return -1;
            r = image[p++];
            v = image[p++];
            if (ssg && r < 0x10) {
                int ch;
                if (!ssg_reg(r, v, &ch)) {
                    reject = "Unsafe SSG register or value";
                    return -1;
                }
                ++writes;
                if (!play) {
                    if (r == 7)
                        ssg_mix = 1;
                    if (ch >= 0)
                        ssg_claim |= 1 << ch;
                    if (r == 6)
                        noise_claim = 1;
                } else {
                    if (until(samples))
                        return -1;
                    if (ch >= 0)
                        ssg_touched |= 1 << ch;
                    if (r == 6)
                        noise_changed = 1;
                    if (r == 7 ? mixer_put(v) : put(r, v))
                        return -1;
                    if (r >= 8)
                        ssg_volume[ch] = v;
                    ssg_meter();
                }
                continue;
            }
            if (!fm_reg(r, v)) {
                reject = "Unsupported FM register";
                return -1;
            }
            ++writes;
            if (play) {
                touched |= 1 << (r == 0x28 ? (v & 3) : (r & 3));
                if (until(samples) || put(r, v))
                    return -1;
                if (ui && r == 0x28) {
                    unsigned char keys = v >> 4;
                    meter_level(v & 3, (keys & 1) + ((keys >> 1) & 1) +
                                       ((keys >> 2) & 1) + (keys >> 3));
                    meter_known |= 1 << (v & 3);
                }
            }
            continue;
        }
        if (cmd == 0x61) {
            if (end - p < 2)
                return -1;
            wait = image[p] | ((unsigned int)image[p+1] << 8);
            p += 2;
        } else if (cmd == 0x62)
            wait = 735;
        else if (cmd == 0x63)
            wait = 882;
        else if (cmd >= 0x70 && cmd <= 0x7f)
            wait = (cmd & 15) + 1;
        else {
            reject = "Unsupported VGM command";
            return -1;
        }
        if (wait > MAX_SAMPLES - samples) {
            reject = "Track exceeds 120 seconds";
            return -1;
        }
        samples += wait;
        if (play && until(samples))
            return -1;
    }
    return -1;
}

static int cleanup(void)
{
    unsigned int ch, op;
    int error = 0;
    if (psg) {
        if (touched)
            for (ch = 0; ch < 4; ++ch)
                psg_write(0x9f + (ch << 5));
        return 0;
    }
    for (ch = 0; ch < 3; ++ch)
        if (ssg_touched & (1 << ch)) {
            if (put(8 + ch, 0))
                error = 1;
            ssg_volume[ch] = 0;
        }
    if (noise_changed && put(6, noise_start))
        error = 1;
    if (mixer_ok && mixer_put(mixer_start))
        error = 1;
    ssg_meter();
    for (ch = 0; ch < 3; ++ch)
        if (touched & (1 << ch)) {
            if (put(0x28, ch))
                error = 1;
            for (op = 0; op < 4; ++op)
                if (put(0x40 + 4 * op + ch, 0x7f))
                    error = 1;
        }
    return error;
}

static const char *load_vgm(const char *path)
{
    unsigned int n = 0, i, j;
    unsigned long version, offset, gd3, loop;
    int fd, got;
    struct stat st;

    total = 0;
    open_failure = 0;
    reject = "Invalid VGM header";
    title[0] = 0;
    loop_pos = 0;
    ssg_claim = noise_claim = 0;
    fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        open_failure = 1;
        return "Cannot open track";
    }
    if (fstat(fd, &st) || !S_ISREG(st.st_mode)) {
        close(fd);
        return "Track is not a regular file";
    }
    do {
        got = read(fd, image + n, sizeof(image) - n);
        if (got > 0)
            n += got;
    } while (got > 0 && n <= MAX_FILE);
    close(fd);
    if (n > MAX_FILE)
        return "Track exceeds 8 KiB";
    if (n >= 2 && image[0] == 0x1f && image[1] == 0x8b)
        return "VGZ unsupported; unzip first";
    if (got || n < (psg ? 0x41 : 0x49) || memcmp(image, "Vgm ", 4) || le32(4) != n - 4)
        goto invalid;
    version = le32(8);
    offset = le32(0x34);
    gd3 = le32(0x14);
    loop = le32(0x1c);
    if (version < (psg ? 0x150 : 0x151) || version > 0x171 ||
        offset < (psg ? 0x0c : 0x14) || offset >= n - 0x34)
        goto invalid;
    if (psg) {
        if (le32(0x0c) != 3579545UL || image[0x28] != 6 ||
            image[0x29] || image[0x2a] != 15 || image[0x2b])
            return "Not Tandy PSG VGM at 3.58 MHz";
    } else if (le32(0x44) != 3993600UL)
        return "Not 26K FM VGM at 3.99 MHz";
    first = (unsigned int)offset + 0x34;
    if (ssg)
        for (i = 0x0c; i < first; ++i)
            if ((i < 0x14 || i >= 0x28) &&
                (i < 0x34 || i >= 0x38) &&
                (i < 0x44 || i >= 0x48) && image[i])
                return "Extra chip/header not allowed";
    end = n;
    if (gd3) {
        if (gd3 < first - 0x14 || gd3 > n - 0x20)
            goto invalid;
        end = (unsigned int)gd3 + 0x14;
        if (memcmp(image + end, "Gd3 ", 4) || le32(end + 8) != n - end - 12)
            goto invalid;
    }
    if (loop) {
        if (loop < first - 0x1c || loop >= end - 0x1c)
            goto invalid;
        loop_pos = (unsigned int)loop + 0x1c;
    }
    if (walk(first, end, 0))
        goto invalid;

    if (gd3 && ui) {
        /* English GD3 title only; display ASCII or fall back to filename. */
        for (i = end + 12, j = 0; i + 1 < n && j < sizeof(title)-1; i += 2) {
            if (!image[i+1] && !image[i])
                break;
            if (image[i+1] || image[i] < 32 || image[i] > 126) {
                j = 0;
                break;
            }
            title[j++] = image[i];
        }
        title[j] = 0;
    }
    return 0;
invalid:
    total = 0;
    return reject;
}

static const char *vgm_hardware(void)
{
    struct utsname os;
    if (uname(&os) ||
        strcmp(os.machine, psg ? "ibmpc i8086" : "pc-98 i8086") ||
        !real_mode())
        return "Wrong machine or not real mode";
    if (psg && !tandy_rom())
        return "Tandy BIOS signature missing";
    if (!psg && ready())
        return "YM2203 not ready; no writes";
    return 0;
}

static const char *play_vgm(void)
{
    const char *err = vgm_hardware();
    int result;
    if (err)
        return err;
    touched = ssg_touched = mixer_ok = noise_changed = 0;
    memset(ssg_volume, 0, sizeof(ssg_volume));
    if (stopped)
        return "Playback canceled";
    if (ssg_claim && ssg_probe())
        return cleanup() ? "YM2203 cleanup incomplete" :
                           "SSG readback/noise refused";
    ssg_meter();
    if (stopped || gettimeofday(&start, 0))
        return cleanup() ? "YM2203 cleanup incomplete" : "Playback start failed";
    result = walk(first, end, 1);
    if (cleanup())
        return "YM2203 cleanup incomplete";
    return result ? "Interrupted; channels muted" : 0;
}

static unsigned long rd32(const unsigned char *p)
{
    return (unsigned long)p[0] | ((unsigned long)p[1] << 8) |
           ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24);
}

/* Drain helper status records; paint at most once per 10 Hz tick. */
static void stream_status(int fd)
{
    static unsigned char rec[STATUS_SIZE];
    static unsigned int have;
    static unsigned char dirty, slice, clock_force;
    unsigned char b[STATUS_SIZE * 3];
    unsigned long tick;
    unsigned int i, ch;
    int got, fresh = 0, clock_total = 0;
    if (fd < 0) {                   /* new helper: forget partial state */
        have = 0;
        dirty = clock_force = 0;
        return;
    }
    while ((got = read(fd, b, sizeof(b))) > 0) {
        for (i = 0; i < (unsigned int)got; ++i) {
            if ((have == 0 && b[i] != 'S') || (have == 1 && b[i] != 'D')) {
                have = b[i] == 'S';   /* resynchronise on the magic */
                continue;
            }
            rec[have++] = b[i];
            if (have < STATUS_SIZE)
                continue;
            have = 0;
            if (rd32(rec + 8) != total) {
                total = rd32(rec + 8);
                clock_total = 1;
            }
            shown = rd32(rec + 4);
            meter_known = rec[2];
            meter_env = rec[3];
            for (ch = 0; ch < STATUS_CHANNELS; ++ch) {
                meters[ch] = rec[12 + ch];
                if (rec[18 + ch] > meter_hi[ch])
                    meter_hi[ch] = rec[18 + ch];   /* peaks from every record */
            }
            fresh = 1;
        }
    }
    /* Paint one slice per poll, round robin: clock, rows 0-2, rows 3-5.
     * A slow poll never starves the later slices. */
    if (fresh) {
        tick = shown / 4410UL;
        if (clock_total || tick != last_tick) {
            last_tick = tick;
            meter_tick();
            dirty = 7;
            clock_force |= clock_total;
        }
    }
    for (i = 0; i < 3 && dirty; ++i) {
        ch = slice;
        slice = slice == 2 ? 0 : slice + 1;
        if (!(dirty & (1 << ch)))
            continue;
        dirty &= ~(1 << ch);
        if (ch == 0) {
            update_clock(shown, clock_force);
            clock_force = 0;
        } else
            update_meter_rows(0, ch == 1 ? 0 : 3, ch == 1 ? 3 : 6);
        fflush(stdout);
        break;
    }
}

static const char *delegate_track(const char *path)
{
    pid_t child, got;
    int status, signaled = 0, pipefd[2] = {-1, -1};

    stream_ready_next = 0;
    if (STREAMING && !pipe(pipefd) &&
        fcntl(pipefd[0], F_SETFL, fcntl(pipefd[0], F_GETFL) | O_NONBLOCK) < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        pipefd[0] = pipefd[1] = -1;
    }
    child = fork();
    if (child == (pid_t)-1) {
        if (pipefd[0] >= 0) {
            close(pipefd[0]);
            close(pipefd[1]);
        }
        return "Cannot start audio helper";
    }
    if (!child) {
        char *args[5];
        signal(SIGTERM, SIG_DFL);
        if (stopped)
            exit(125);
        close(0);
        close(1);
        close(2);
        if (pipefd[1] >= 0) {
            close(pipefd[0]);
            if (pipefd[1] != STATUS_FD) {
                dup2(pipefd[1], STATUS_FD);
                close(pipefd[1]);
            }
        }
        args[0] = pcm ? "/bin/sbpcm" : pcstream ? "/bin/26kplay" :
                  tstream ? "/bin/tandyplay" : "/bin/opl2play";
        args[1] = pcm ? (char *)path : pcstream ? "--pc98-26k" :
                  tstream ? "--tandy-ex" : "--adlib-388";
        args[2] = pcm ? 0 : (char *)path;
        args[3] = pcstream ? cue_files[selected] : 0;
        args[4] = 0;
        execv(args[0], args);
        exit(127);
    }
    if (pipefd[1] >= 0)
        close(pipefd[1]);
    stream_status(-1);
    for (;;) {
        got = waitpid(child, &status, WNOHANG);
        if (got == child)
            break;
        stream_status(pipefd[0]);
        if (got == (pid_t)-1 && errno != EINTR)
            return "Audio helper wait failed";
        if (!stopped)
            input();
        if (stopped && !signaled) {
            kill(child, SIGTERM);
            signaled = 1;
        }
        usleep(30000);
    }
    if (pipefd[0] >= 0)
        close(pipefd[0]);
    if (!WIFEXITED(status))
        return signaled ? "Stopped" : "Audio helper aborted";
    status = WEXITSTATUS(status);
    if (status == 127)
        return "Missing helper in /bin";
    if (status == 0) {
        stream_ready_next = 1;
        return 0;
    }
    if (STREAMING) {
        if (status == 2)
            return pcstream ? "Unsupported VGM or cue" : "Unsupported Tandy PSG VGM";
        if (status == 3 && signaled) {
            stream_ready_next = 1;
            return "Stopped; channels muted";
        }
        return pcstream ? "26K helper playback failed" : "PSG helper playback failed";
    }
    if (pcm) {
        if (status == 1)
            return "Stopped; PCM reset";
        if (status == 2)
            return "Cannot open WAV";
        if (status == 3)
            return "Need 8-bit mono PCM WAV";
        if (status == 4)
            return "Cannot open /dev/dsp";
        return "PCM I/O or halt failed";
    }
    if (status == 2)
        return "Unsupported OPL2 VGM";
    return signaled ? "Stopped; helper exited" : "OPL2 hardware/playback failed";
}

static int run_deck(void)
{
    const char *loaded, *result;
    int reload = 1, autoplay = 0;
    if (term_on()) {
        fputs("Signal Deck needs an ANSI ELKS console.\n", stderr);
        return 1;
    }
    for (;;) {
        if (signal_stop || (stopped && !action))
            break;
        if (reload) {
            shown = 0;
            if (STREAMING)
                total = 0;          /* the helper reports it during play */
            loaded = (pcm || opl || STREAMING) ? 0 : load_vgm(files[selected]);
            note = loaded ? loaded : "Ready";
            reload = 0;
            dashboard();
        }
        if (!autoplay)
            input();
        if (signal_stop || action == 'q' || (stopped && !action))
            break;
        if (action == 'p' || action == 'n') {
            selected = (selected + count + (action == 'p' ? -1 : 1)) % count;
            action = 0;
            stopped = 0;
            reload = 1;
            continue;
        }
        if (action == 's') {
            action = 0;
            stopped = 0;
            note = "Stopped";
            dashboard();
        }
        if (action == ' ' || autoplay) {
            autoplay = 0;
            action = 0;
            if (!loaded) {
                active = 1;
                shown = 0;
                last_tick = 0;
                if (STREAMING)
                    total = 0;      /* the helper reports it */
                next_key_poll = 0;
                note = pcm ? "Streaming to /dev/dsp" : opl ? "Direct to OPL2 at 388h" : pcstream ? "Streaming to YM2203" : tstream ? "Streaming to PSG" : "Direct to sound chip";
                dashboard();
                result = pcm || opl || STREAMING ? delegate_track(files[selected]) : play_vgm();
                active = 0;
                meters_clear();
                if (signal_stop || action == 'q' || (stopped && !action))
                    break;
                if (action == 'p' || action == 'n') {
                    if (!STREAMING || stream_ready_next) {
                        autoplay = 1;
                        continue;
                    }
                    action = 0;
                    stopped = 0;
                    note = result ? result : "Next canceled (helper status)";
                    dashboard();
                    continue;
                }
                if (action == 's') {
                    action = 0;
                    stopped = 0;
                    note = result ? result : "Stopped";
                } else {
                    note = result ? result : pcm ? "Playback complete" : "Complete; channels muted";
                    if (!result)
                        shown = total;
                }
                dashboard();
            }
        }
        usleep(30000);
    }
    term_off();
    return 0;
}

int main(int argc, char **argv)
{
    const char *err;
    int arg = 1, i;

    if (argc > 1 && !strcmp(argv[1], "--deck")) {
        ui = 1;
        ++arg;
    }
    if (argc - arg < 2 || (!ui && argc - arg != 2))
        goto usage;
    if (!strcmp(argv[arg], "--tandy-ex")) {
        psg = 1;
        chip = "TANDY / SN76489";
    } else if (!strcmp(argv[arg], "--pc98-26k"))
        chip = "PC-98 / YM2203 FM";
    else if (!strcmp(argv[arg], "--pc98-26ks")) {
        ssg = 1;
        chip = "PC-98 / 26K FM+SSG";
    } else if (ui && !strcmp(argv[arg], "--tandy-stream")) {
        tstream = 1;
        chip = "TANDY / PSG STREAM";
    } else if (ui && !strcmp(argv[arg], "--pc98-stream")) {
        pcstream = 1;
        chip = "PC-98 / 26K STREAM";
    } else if (ui && !strcmp(argv[arg], "--sb-pcm")) {
        pcm = 1;
        chip = "SOUND BLASTER / PCM";
    } else if (ui && !strcmp(argv[arg], "--adlib-388")) {
        opl = 1;
        chip = "ADLIB / YM3812 FM";
    } else
        goto usage;

    if (signal(SIGINT, on_signal) == SIG_ERR ||
        signal(SIGTERM, on_signal) == SIG_ERR ||
        signal(SIGHUP, on_signal) == SIG_ERR)
        return 1;
    if (ui) {
        files = argv + arg + 1;
        if (pcstream) {
            /* The explicit cue belongs only to its immediately preceding track. */
            for (i = arg + 1; i < argc; ++i) {
                if (count == 99 || !*argv[i] || !strncmp(argv[i], "--", 2))
                    goto usage;
                files[count] = argv[i];
                if (i + 1 < argc && !strcmp(argv[i+1], "--percussion")) {
                    i += 2;
                    if (i >= argc || !*argv[i] || !strncmp(argv[i], "--", 2))
                        goto usage;
                    cue_files[count] = argv[i];
                }
                ++count;
            }
        } else {
            count = argc - arg - 1;
            if (count > 99)
                goto usage;
        }
        return run_deck();
    }
    err = load_vgm(argv[arg+1]);
    if (err) {
        fprintf(stderr, "%s\n", err);
        return open_failure ? 1 : 2;
    }
    err = play_vgm();
    if (err) {
        fprintf(stderr, "%s\n", err);
        return 1;
    }
    puts("Playback complete; channels muted");
    return 0;
usage:
    fputs("Usage: elksvgm [--deck] --pc98-26k|--pc98-26ks|--tandy-ex FILE.VGM [more with --deck]\n"
          "       elksvgm --deck --sb-pcm FILE.WAV [more]\n"
          "       elksvgm --deck --adlib-388 FILE.VGM [more]\n"
          "       elksvgm --deck --pc98-stream FILE.VGM [--percussion FILE.CUE] [more]\n"
          "       elksvgm --deck --tandy-stream FILE.VGM [more]\n",
          stderr);
    return 2;
}
