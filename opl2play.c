/* One-shot melodic VGM player for an idle AdLib-compatible OPL2 on
 * real-mode IBM PC ELKS. Fully validate before touching the I/O ports.
 * One physical chip, no timers/CSM/rhythm/OPL3, PCM, or loop replay.
 */
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <sys/utsname.h>
#include <unistd.h>

#define FM_ADDR 0x388
#define FM_DATA 0x389
#define MAX_FILE 8192U
#define MAX_SAMPLES (120UL * 44100UL)

static unsigned char image[MAX_FILE + 1];
static volatile unsigned char stopped;
static unsigned int touched;
static unsigned char last_b[9];
static struct timeval start;
static unsigned int loop_pos;

static void on_signal(int sig)
{
    (void)sig;
    stopped = 1;
}

static unsigned long le32(unsigned int p)
{
    return (unsigned long)image[p] |
           ((unsigned long)image[p+1] << 8) |
           ((unsigned long)image[p+2] << 16) |
           ((unsigned long)image[p+3] << 24);
}

static unsigned char status(void)
{
    unsigned char s;
    asm volatile ("inb %w1,%0" : "=a" (s) : "d" (FM_ADDR));
    return s;
}

static void put(unsigned char r, unsigned char v)
{
    unsigned int i;
    /* AdLib timing is traditionally achieved by 6 address-port reads
     * after the address and 35 after data. Use larger counts, not CPU
     * speed-dependent NOPs; assumes ISA-style I/O timing. */
    asm volatile ("outb %0,%w1" :: "a" (r), "d" (FM_ADDR));
    for (i = 0; i < 16; ++i)
        (void)status();
    asm volatile ("outb %0,%w1" :: "a" (v), "d" (FM_DATA));
    for (i = 0; i < 80; ++i)
        (void)status();
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

/* Both operators of channels 0..8 are interleaved in these banks. */
static const signed char operator_channel[32] = {
    0, 1, 2, 0, 1, 2, -1, -1,
    3, 4, 5, 3, 4, 5, -1, -1,
    6, 7, 8, 6, 7, 8, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1
};

static int fm_reg(unsigned char r, unsigned char v, int *ch)
{
    *ch = -1;
    if (r == 0x01)                     /* waveform enable only */
        return v == 0 || v == 0x20;
    if (r >= 0xa0 && r <= 0xa8)
        *ch = r - 0xa0;
    else if (r >= 0xb0 && r <= 0xb8 && v <= 0x3f)
        *ch = r - 0xb0;
    else if (r >= 0xc0 && r <= 0xc8 && v <= 0x0f)
        *ch = r - 0xc0;
    else if ((r >= 0x20 && r <= 0x9f) ||
             (r >= 0xe0 && v <= 3))
        *ch = operator_channel[r & 31];
    return *ch >= 0;
}

static int single_chip(unsigned int first)
{
    /* Accept only the musical clock declared for YM3812. This bounded
     * subset refuses other clocks, chip overrides, and nonzero extension
     * fields rather than guessing which later header fields are safe. */
    unsigned int j;
    for (j = 0x0c; j < 0x14; ++j)
        if (image[j])
            return 0;
    for (j = 0x28; j < 0x50; ++j)
        if ((j < 0x34 || j >= 0x38) && image[j])
            return 0;
    for (j = 0x54; j < first; ++j)
        if (image[j])
            return 0;
    return 1;
}

/* Absolute schedule: round to the first available ELKS tick, not per wait.
 * Chunking also keeps surplus-sleep and signal response bounded.
 */
static int until(unsigned long samples)
{
    struct timeval now;
    long secs, elapsed, deadline;
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
            return 0;
        if (deadline - elapsed > 100000L)
            usleep(100000);
        else
            usleep(deadline - elapsed);
    }
    return -1;
}

static int walk(unsigned int p, unsigned int end, int play)
{
    unsigned long samples = 0;
    unsigned int wait;
    unsigned char cmd, r, v;
    int writes = 0, ch;
    int loop_seen = !loop_pos;

    while (p < end && !stopped) {
        if (p == loop_pos)
            loop_seen = 1;
        cmd = image[p++];
        if (cmd == 0x66)
            return (p == end && writes && samples && loop_seen) ? 0 : -1;
        if (cmd == 0x5a) {
            if (end - p < 2)
                return -1;
            r = image[p++];
            v = image[p++];
            if (!fm_reg(r, v, &ch))
                return -1;
            if (ch >= 0)
                ++writes;
            if (play) {
                if (until(samples))
                    return -1;
                if (ch >= 0) {
                    touched |= 1U << ch;
                    if (r >= 0xb0 && r <= 0xb8)
                        last_b[ch] = v;
                }
                put(r, v);
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
        else
            return -1;
        if (wait > MAX_SAMPLES - samples)
            return -1;
        samples += wait;
        if (play && until(samples))
            return -1;
    }
    return -1;
}

static void cleanup(void)
{
    unsigned int ch, op;
    for (ch = 0; ch < 9; ++ch)
        if (touched & (1U << ch))
            put(0xb0 + ch, last_b[ch] & 0x1f);
    for (op = 0; op < sizeof(operator_channel); ++op) {
        int c = operator_channel[op];
        if (c >= 0 && (touched & (1U << c))) {
            /* Maximum attenuation plus fastest release, including
             * additive and sustained voices. */
            put(0x40 + op, 0x3f);
            put(0x80 + op, 0x0f);
        }
    }
}

int main(int argc, char **argv)
{
    unsigned int n = 0, first, end;
    unsigned long version, offset, gd3, loop;
    struct utsname os;
    int fd, got, result, check;

    check = argc == 3 && !strcmp(argv[1], "--check");
    if (!check && (argc != 3 || strcmp(argv[1], "--adlib-388"))) {
        fputs("Usage: elksvgm-opl2 --check|--adlib-388 FILE.VGM (idle confirmed OPL2-compatible hardware)\n",
              stderr);
        return 2;
    }
    fd = open(argv[2], O_RDONLY);
    if (fd < 0) {
        perror(argv[2]);
        return 1;
    }
    do {
        got = read(fd, image + n, sizeof(image) - n);
        if (got > 0)
            n += got;
    } while (got > 0 && n <= MAX_FILE);
    close(fd);
    if (got || n < 0x55 || memcmp(image, "Vgm ", 4) || le32(4) != n - 4)
        goto invalid;
    version = le32(8);
    offset = le32(0x34);
    gd3 = le32(0x14);
    loop = le32(0x1c);
    if (version < 0x151 || version > 0x171 ||
        offset < 0x20 || offset >= n - 0x34 ||
        le32(0x50) != 3579545UL)
        goto invalid;
    first = (unsigned int)offset + 0x34;
    if (!single_chip(first))
        goto invalid;
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

    if (check)
        return 0;

    if (signal(SIGINT, on_signal) == SIG_ERR ||
        signal(SIGTERM, on_signal) == SIG_ERR)
        return 1;
    if (uname(&os) ||
        strcmp(os.machine, "ibmpc i8086") ||
        !real_mode()) {
        fputs("Wrong machine or not a real-mode ELKS kernel; no I/O writes\n",
              stderr);
        return 1;
    }
    /* This only detects a likely absent/floating port, not a chip.
     * There is no automatic or timer-based detection. */
    if (stopped || (status() == 0xff && status() == 0xff)) {
        fputs("OPL2 port not responding or playback cancelled; no writes\n", stderr);
        return 1;
    }
    if (gettimeofday(&start, 0))
        return 1;
    result = walk(first, end, 1);
    cleanup();
    if (result) {
        fputs("Playback did not complete; channels muted\n", stderr);
        return 1;
    }
    puts("Playback complete; channels muted");
    return 0;
invalid:
    fputs("Unsupported VGM (raw melodic single YM3812, 3.579545 MHz, max 8KiB)\n",
          stderr);
    return 2;
}
