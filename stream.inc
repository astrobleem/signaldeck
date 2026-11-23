/* Chip-independent VGM streaming core shared by 26kplay and tandyplay.
 *
 * Before including, define MAX_FILE and MAX_SAMPLES. After including,
 * define the chip hooks declared below:
 *   chip_command  one VGM command byte that is not a wait or the end:
 *                 return 1 for an accepted chip write (written when play),
 *                 0 when cmd is not a chip command, -1 to refuse.
 *   chip_next_event  earliest time (us since start) another device needs
 *                 service, or NO_EVENT.
 *   chip_loop_state  save (0, preflight at the loop point) or restore (1,
 *                 replay seam) any parse state chip_command keeps, so the
 *                 replay is checked exactly as the preflight was.
 * The score is validated in full before any hardware access. While it is
 * validated it is also copied to far memory when ELKS can spare it, so
 * playback and loop replays never wait for the disk; otherwise playback
 * streams it from the file in 1 KiB reads. Playback runs on an absolute
 * schedule and replays from the loop point when asked. */
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <sys/utsname.h>
#include <unistd.h>

#define MAX_GD3 4096UL
#ifndef NO_EVENT
#define NO_EVENT 0x7fffffffL
#endif

static unsigned char image[1024];
static unsigned int cur, have;      /* next byte and fill of image[] */
static unsigned long buf_pos;       /* file offset of image[0] */
static unsigned int loop_idx = 0xffff; /* loop point's index in image[] */
static volatile unsigned char stopped;
static unsigned int far_seg;        /* far copy of the command data */
static unsigned long far_base;      /* its file offset */
static unsigned char far_state, want_far; /* 1 filling, 2 complete */
static struct timeval start;
static unsigned long loop_pos, loop_samples, time_base, pass_total;
static unsigned char looping;

struct vgm {
    unsigned long size, first, data_end, expect, loop_len, version;
};

static int chip_command(int cmd, int fd, unsigned long end, int play);
static long chip_next_event(long elapsed);
static void chip_loop_state(int restore);

#include "status.inc"
#include "ptime.inc"

/* The playback clock: the sub-tick PIT clock when /dev/kmem allows it,
 * else gettimeofday at ELKS tick resolution. */
static unsigned char precise;

static int clock_start(int pc98)
{
    precise = !ptime_init(pc98);
    if (precise) {
        ptime_start();
        return 0;
    }
    return gettimeofday(&start, 0);
}

/* Microseconds since clock_start, or -1. */
static long clock_now(void)
{
    struct timeval now;
    long secs;
    if (precise)
        return ptime_us();
    if (gettimeofday(&now, 0))
        return -1;
    secs = now.tv_sec - start.tv_sec;
    if (secs < 0 || secs > 2000L)
        return -1;
    return secs * 1000000L + now.tv_usec - start.tv_usec;
}

static void clock_shift(long secs)
{
    if (precise)
        pt_off += secs * 1000000L;
    else
        start.tv_sec += secs;
}

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

static int real_mode(void)
{
    unsigned int f;
    asm volatile ("pushf; popw %0" : "=a" (f));
    if ((f & 0xf000) == 0xf000)
        return 1;                 /* 8086/186 flags, no SMSW instruction */
    asm volatile (".byte 0x0f,0x01,0xe0" : "=a" (f)); /* SMSW AX on 286+ */
    return !(f & 1);
}

/* Absolute schedule. With the PIT clock, sleep to within one tick of the
 * deadline and spin the rest; otherwise round to the first ELKS tick. */
static int until(unsigned long samples)
{
    long elapsed, deadline, next;
    int reported = 0;
    deadline = (long)((samples / 441) * 10000UL +
                      ((samples % 441) * 10000UL + 440UL) / 441);
    while (!stopped) {
        elapsed = clock_now();
        /* At most 60 s of rebase residue plus a loop pass and a pass. */
        if (elapsed < 0 ||
            elapsed / 1000000L > (long)(2 * (MAX_SAMPLES / 44100UL)) + 100L)
            return -1;
        next = chip_next_event(elapsed);
        if (elapsed >= deadline)
            return 0;
        if (next < deadline) next -= elapsed;
        else next = deadline - elapsed;
        /* Status only uses a full tick of slack, once per wait. */
        if (!reported && next >= 10000L) {
            unsigned long at = ((unsigned long)elapsed / 1000UL * 441UL) / 10UL;
            reported = 1;
            if (status_due(elapsed, at > time_base ? at - time_base : 0, pass_total))
                continue;
        }
        if (precise) {
            if (next < 15000L)
                continue;          /* spin the last stretch on the PIT */
            next -= 10000L;        /* a sleep can end up to a tick late */
        }
        usleep(next > 100000L ? 100000L : next);
    }
    return -1;
}

/* Copy n bytes between image[] and the far copy with REP MOVSB. */
static void far_copy(int to_far, unsigned long off, unsigned int n)
{
    unsigned int seg = far_seg + (unsigned int)(off >> 4);
    unsigned int o = (unsigned int)off & 15;
    unsigned char *near = image;
    if (to_far)
        asm volatile ("push %%es\n\tmov %%ax,%%es\n\tcld\n\trep movsb\n\tpop %%es"
                      : "+S" (near), "+D" (o), "+c" (n) : "a" (seg) : "memory");
    else
        asm volatile ("push %%ds\n\tpush %%es\n\tpush %%ds\n\tpop %%es\n\t"
                      "mov %%ax,%%ds\n\tcld\n\trep movsb\n\tpop %%es\n\tpop %%ds"
                      : "+S" (o), "+D" (near), "+c" (n) : "a" (seg) : "memory");
}

static void far_release(void)
{
    if (far_seg)
        fmemfree((void __far *)((unsigned long)far_seg << 16));
    far_seg = 0;
    far_state = 0;
}

/* Load the next block after image[], from far memory or the file. */
static int refill(int fd, unsigned long end)
{
    unsigned long p = buf_pos + have;
    unsigned int count;
    int got;
    if (p >= end) return -1;
    count = end-p > sizeof(image) ? sizeof(image) : (unsigned int)(end-p);
    if (far_state == 2) {
        far_copy(0, p - far_base, count);
        got = (int)count;
    } else {
        do { got = read(fd, image, count); } while (got < 0 && errno == EINTR && !stopped);
        if (got <= 0) return -1;
        if (far_state == 1)
            far_copy(1, p - far_base, (unsigned int)got);
    }
    buf_pos = p;
    have = (unsigned int)got;
    cur = 0;
    loop_idx = loop_pos >= p && loop_pos < p + have ? (unsigned int)(loop_pos - p) : 0xffff;
    return 0;
}

static int seek_to(int fd, unsigned long p)
{
    if (far_state != 2 && lseek(fd, (long)p, SEEK_SET) != (long)p) return -1;
    buf_pos = p;
    cur = have = 0;
    loop_idx = 0xffff;
    return 0;
}

static int getbyte(int fd, unsigned long end)
{
    if (cur == have && refill(fd, end)) return -1;
    return image[cur++];
}

/* Inline fast path for the byte-at-a-time parse. */
#define GETB(v) do { if (cur != have) (v) = image[cur++]; \
                     else (v) = getbyte(fd, end); } while (0)

static int walk(int fd, const struct vgm *g, int play)
{
    unsigned long samples=0, cmds=0, writes=0, end=g->data_end, expect=g->expect;
    unsigned long limit=end-g->first;
    unsigned int wait;
    int cmd, r, v, loopseen=!loop_pos;
    if (seek_to(fd, g->first)) return -1;
    /* Every command is at least one byte, so this bounds a pass. */
    while (!stopped && ++cmds<=limit) {
        if (cur == have && refill(fd, end)) return -1;
        if (cur == loop_idx) {
            if (!play) {
                loop_samples=samples;
                chip_loop_state(0);
            }
            loopseen=1;
        }
        cmd=image[cur++];
        if (cmd==0x66) {
            if (buf_pos+cur!=end || samples!=expect || !writes || !loopseen) return -1;
            if (!play || !looping) return 0;
            /* Replay from the validated loop point on the same absolute
             * schedule. Whole seconds move into start, so no drift. */
            time_base+=expect-loop_samples;
            if (time_base>=60UL*44100UL) {
                clock_shift((long)(time_base/44100UL));
                time_base%=44100UL;
            }
            if (seek_to(fd, loop_pos)) return -1;
            samples=loop_samples; cmds=0;
            chip_loop_state(1);
            continue;
        }
        r = chip_command(cmd, fd, end, play);
        if (r < 0) return -1;
        if (r) {
            ++writes;
            continue;
        }
        if (cmd==0x61) {
            GETB(r); GETB(v);
            if (r<0 || v<0) return -1;
            wait=r|((unsigned)v<<8);
        } else if (cmd==0x62) wait=735;
        else if (cmd==0x63) wait=882;
        else if (cmd>=0x70 && cmd<=0x7f) wait=(cmd&15)+1;
        else return -1;
        if (wait > expect-samples) return -1;
        samples+=wait;
        if (play && until(time_base+samples)) return -1;
    }
    return -1;
}

/* Validate, never display: "Gd3 " v1.00, exact length to EOF, and exactly
 * eleven NUL-terminated UTF-16 strings. */
static int gd3_check(int fd, unsigned long at, unsigned long size)
{
    unsigned long len, done;
    unsigned int n, i, nuls=0, last=0;
    int got;
    if (lseek(fd,(long)at,SEEK_SET)!=(long)at) return -1;
    for (n=0; n<12; n+=got)
        if ((got=read(fd,image+n,12-n))<=0) return -1;
    if (memcmp(image,"Gd3 ",4) || le32(4)!=0x100UL) return -1;
    len=le32(8);
    if (len!=size-at-12 || len<22 || len>MAX_GD3 || (len&1)) return -1;
    for (done=0; done<len; done+=n) {
        n=len-done>sizeof(image)?sizeof(image):(unsigned int)(len-done);
        for (i=0; i<n; i+=got)
            if ((got=read(fd,image+i,n-i))<=0) return -1;
        for (i=0; i<n; i+=2) {
            last=!image[i] && !image[i+1];
            nuls+=last;
        }
    }
    return nuls==11 && last ? 0 : -1;
}

/* Read the header (at most 256 bytes, up to the data) into image and
 * check what every stream shares; the caller checks its chip fields from
 * image, then calls vgm_body. */
static int vgm_header(int fd, struct vgm *g, unsigned int *hdr)
{
    long len=lseek(fd,0L,SEEK_END);
    unsigned int n=0;
    int got;
    if (len<0x41 || (unsigned long)len>MAX_FILE || lseek(fd,0L,SEEK_SET)!=0L)
        return -1;
    g->size=(unsigned long)len;
    while (n<0x40 && (got=read(fd,image+n,0x40-n))>0) n+=got;
    if (n!=0x40 || memcmp(image,"Vgm ",4) || le32(4)!=g->size-4) return -1;
    g->version=le32(8);
    if (g->version<0x150 || g->version>0x171) return -1;
    g->first=le32(0x34);
    if (g->first<0x0c || g->first>=g->size-0x34) return -1;
    g->first+=0x34;
    *hdr = g->first > 256 ? 256 : (unsigned int)g->first;
    while (n<*hdr && (got=read(fd,image+n,*hdr-n))>0) n+=got;
    if (n!=*hdr) return -1;
    g->expect=le32(0x18);
    g->loop_len=le32(0x20);
    return g->expect && g->expect<=MAX_SAMPLES ? 0 : -1;
}

/* GD3 and loop bounds, then the full validating walk. Uses image. */
static int vgm_body(int fd, struct vgm *g)
{
    unsigned long gd3=le32(0x14);
    loop_pos=le32(0x1c);
    g->data_end=g->size;
    if (gd3) {
        /* The tag must start after at least the end command. */
        if (gd3>g->size-0x14-12 || gd3+0x14<g->first+1) return -1;
        g->data_end=gd3+0x14;
    }
    if (loop_pos) {
        if (loop_pos<g->first-0x1c || loop_pos>=g->data_end-0x1c) return -1;
        loop_pos+=0x1c;
    }
    if (gd3 && gd3_check(fd,g->data_end,g->size))
        return -1;
    /* Best effort: copy the commands to far memory while checking them. */
    if (want_far) {
        void __far *m = fmemalloc(g->data_end - g->first);
        if (m && m != (void __far *)-1L) {
            far_seg = (unsigned int)((unsigned long)m >> 16);
            far_base = g->first;
            far_state = 1;
        }
    }
    if (walk(fd,g,0)) {
        far_release();
        return -1;
    }
    if (far_state == 1)
        far_state = 2;
    pass_total=g->expect;
    return 0;
}

/* A loop plays only when the header's loop length agrees with the score. */
static int loop_ok(const struct vgm *g)
{
    return loop_pos && g->loop_len==g->expect-loop_samples;
}
