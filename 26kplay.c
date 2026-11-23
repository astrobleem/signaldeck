#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <sys/utsname.h>
#include <unistd.h>

#define FM_ADDR 0x188
#define FM_DATA 0x18a
#define MAX_FILE (256UL * 1024UL)
#define MAX_SAMPLES (300UL * 44100UL)
#define MAX_GD3 4096UL

static unsigned char image[1024];
static unsigned int cur, have;
static unsigned char savedmix, touchssg;
static unsigned char savedenv[3], envsaved;
static unsigned long pos;
static volatile unsigned char stopped;
static unsigned char touched, ssgeg, mode3;
static struct timeval start;
static unsigned long loop_pos, loop_samples, time_base;
static unsigned char looping;

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

static int ssg_read(unsigned char r, unsigned char *value)
{
    if (ready()) return -1;
    asm volatile ("outb %0,%w1" :: "a" (r), "d" (FM_ADDR));
    if (ready()) return -1;
    asm volatile ("inb %w1,%0" : "=a" (*value) : "d" (FM_DATA));
    return 0;
}

/* Timer/IRQ controls (0x24-0x26, 0x27 timer bits), the other prescaler
 * selects (0x2e/0x2f), SSG I/O ports (0x0e/0x0f) and unlisted registers
 * stay refused. */
static int fm_reg(unsigned char r, unsigned char v)
{
    if (r == 0x28)
        return !(v & 0x0c) && (v & 3) != 3;
    if (r == 0x27)
        return v == 0x40;          /* FM3 special mode, timers left stopped */
    if (r == 0x2d)
        return v == 0;             /* reselects the reset-default prescaler */
    if (r <= 7)
        return (r != 1 && r != 3 && r != 5) || v <= 15;
    if (r >= 8 && r <= 10)
        return v <= 15 || v == 0x10; /* fixed level or hardware envelope */
    if (r == 11 || r == 12)
        return 1;                  /* envelope period */
    if (r == 13)
        return v <= 15;            /* envelope shape */
    if ((r & 3) == 3)
        return 0;
    return (r >= 0x30 && r <= 0x9e) ||
           (r >= 0xa0 && r <= 0xa6) ||
           (r >= 0xa8 && r <= 0xae) || /* FM3 slot frequencies */
           (r >= 0xb0 && r <= 0xb2);
}

#include "overlay.inc"
#include "status.inc"

static unsigned char ssg_mix = 0x3f, ssg_vol[3];

/* Report what the chip was told, as the direct player does: FM keyed
 * operators per channel and SSG level per enabled voice. */
static void status_write(unsigned char r, unsigned char v)
{
    unsigned char ch, vol;
    if (!st_on)
        return;
    if (r == 0x28) {
        ch = v & 3;
        v >>= 4;
        status_level(ch, (v & 1) + ((v >> 1) & 1) + ((v >> 2) & 1) + (v >> 3), 0);
        return;
    }
    if (r == 7)
        ssg_mix = v;
    else if (r >= 8 && r <= 10)
        ssg_vol[r - 8] = v;
    else if (r >= 6)
        return;
    for (ch = 0; ch < 3; ++ch) {
        if (!(st_known & (8 << ch)) && r != 8 + ch && (r >> 1) != ch)
            continue;
        vol = ssg_vol[ch];
        if (vol & 0x10)       /* an envelope sounds even with tone and noise off */
            status_level(3 + ch, 15, 1);
        else if ((ssg_mix & (1 << ch)) && (ssg_mix & (8 << ch)))
            status_level(3 + ch, 0, 0);     /* fixed level alone is only DC */
        else
            status_level(3 + ch, vol & 15, 0);
    }
}

static unsigned long pass_total;

static int until(unsigned long samples)
{
    struct timeval now;
    long secs, elapsed, deadline, next;
    int reported = 0;
    deadline = (long)((samples / 441) * 10000UL +
                      ((samples % 441) * 10000UL + 440UL) / 441);
    while (!stopped) {
        if (gettimeofday(&now, 0))
            return -1;
        secs = now.tv_sec - start.tv_sec;
        if (secs < 0 || secs > 700) /* < 60 s rebase residue + two passes */
            return -1;
        elapsed = secs * 1000000L + now.tv_usec - start.tv_usec;
        if (elapsed < 0)
            return -1;
        next=overlay_step(elapsed);
        if (elapsed >= deadline)
            return 0;
        if (next<deadline) next-=elapsed;
        else next=deadline-elapsed;
        /* Status only uses a full tick of slack, once per wait. */
        if (!reported && next >= 10000L) {
            unsigned long at = ((unsigned long)elapsed / 1000UL * 441UL) / 10UL;
            reported = 1;
            if (status_due(elapsed, at > time_base ? at - time_base : 0, pass_total))
                continue;
        }
        if (next > 100000L)
            usleep(100000);
        else
            usleep(next);
    }
    return -1;
}

static int getbyte(int fd, unsigned long end)
{
    int got;
    if (pos >= end) return -1;
    if (cur == have) {
        unsigned int count = end-pos > sizeof(image) ? sizeof(image) : (unsigned int)(end-pos);
        do { got = read(fd, image, count); } while (got < 0 && errno == EINTR && !stopped);
        if (got <= 0) return -1;
        have = (unsigned int)got;
        cur = 0;
    }
    ++pos;
    return image[cur++];
}

static int walk(int fd, unsigned long first, unsigned long end,
                unsigned long expect, int play)
{
    unsigned long samples=0, cmds=0, writes=0;
    unsigned int wait;
    int cmd,r,v,mixready=0, loopseen=!loop_pos;
    if (lseek(fd, (long)first, SEEK_SET) != (long)first) return -1;
    cur=have=0; pos=first;
    while (pos<end && !stopped && ++cmds<=200000UL) {
        if (pos==loop_pos) {
            if (!play) loop_samples=samples;
            loopseen=1;
        }
        cmd=getbyte(fd,end);
        if (cmd==0x66) {
            if (pos!=end || samples!=expect || !writes || !loopseen) return -1;
            if (!play || !looping) return 0;
            /* Replay from the validated loop point on the same absolute
             * schedule. Whole seconds move into start, so no drift. */
            time_base+=expect-loop_samples;
            if (time_base>=60UL*44100UL) {
                start.tv_sec+=(long)(time_base/44100UL);
                time_base%=44100UL;
            }
            if (lseek(fd,(long)loop_pos,SEEK_SET)!=(long)loop_pos) return -1;
            cur=have=0; pos=loop_pos; samples=loop_samples; cmds=0;
            continue;
        }
        if (cmd==0x55) {
            r=getbyte(fd,end); v=getbyte(fd,end);
            if (r<0 || v<0 || !fm_reg(r,v)) return -1;
            ++writes;
            if (play) {
                if (r<=13) {
                    if (!mixready && ssg_read(7,&savedmix)) return -1;
                    mixready=touchssg=1;
                    if (r>=11 && !envsaved) {
                        if (ssg_read(11,savedenv) || ssg_read(12,savedenv+1) ||
                            ssg_read(13,savedenv+2)) return -1;
                        envsaved=1;
                    }
                    if (r==7) v=(v&0x3f)|(savedmix&0xc0);
                } else if (r==0x28) touched|=1<<(v&3);
                else if (r==0x27) mode3=1;
                else if (r>=0x30) {
                    unsigned char ch=(r>=0xa8&&r<=0xae)?2:(r&3);
                    touched|=1<<ch;
                    if (r>=0x90 && r<=0x9e) ssgeg|=1<<ch;
                }
                if (put(r,v)) return -1;
                status_write(r,v);
            }
            continue;
        }
        if (cmd==0x61) {
            r=getbyte(fd,end); v=getbyte(fd,end);
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

/* Mute, then return the modes this score switched on to reset defaults so
 * the next track does not inherit FM3 split mode, SSG-EG or an envelope. */
static int cleanup(void)
{
    unsigned int ch, op;
    int error=0;
    for (ch=0;ch<3;++ch)
        if (touched&(1<<ch)) {
            if (put(0x28,ch)) error=1;
            for (op=0;op<4;++op) {
                if (put(0x40+4*op+ch,0x7f)) error=1;
                if ((ssgeg&(1<<ch)) && put(0x90+4*op+ch,0)) error=1;
            }
        }
    if (mode3 && put(0x27,0)) error=1;
    if (touchssg) {
        for (ch=8;ch<=10;++ch)
            if (put(ch,0)) error=1;
        if (envsaved && (put(11,savedenv[0]) || put(12,savedenv[1]) ||
                         put(13,savedenv[2]&15))) error=1;
        if (put(7,savedmix)) error=1;
    }
    return error;
}

int main(int argc, char **argv)
{
    struct utsname os;
    unsigned long size, first, expect, clock, i, version, gd3, data_end, loop_len;
    long len;
    int fd, n=0, got, result, check;
    check=argc>1 && !strcmp(argv[1],"--check");
    if ((argc!=3 && argc!=4) || (!check && strcmp(argv[1],"--pc98-26k"))) {
        fputs("Usage: 26kplay --pc98-26k|--check FILE.VGM [FILE.CUE] (idle reset-default 26/K)\n",stderr);
        return 2;
    }
    if (!check && (uname(&os)||strcmp(os.machine,"pc-98 i8086")||!real_mode())) {
        fputs("Requires real-mode PC-98 ELKS; no I/O performed\n",stderr);
        return 1;
    }
    fd=open(argv[2],O_RDONLY);
    if (fd<0) { perror(argv[2]); return 1; }
    len=lseek(fd,0L,SEEK_END);
    if (len<129 || (unsigned long)len>MAX_FILE || lseek(fd,0L,SEEK_SET)!=0L)
        goto invalid;
    size=(unsigned long)len;
    while (n<128 && (got=read(fd,image+n,128-n))>0) n+=got;
    if (n!=128||memcmp(image,"Vgm ",4)||le32(4)!=size-4)
        goto invalid;
    version=le32(8);
    first=le32(0x34);
    if (version<0x151 || version>0x171 || first!=0x4c) goto invalid;
    first+=0x34;
    clock=le32(0x44);
    if (clock!=3993600UL && clock!=4000000UL) goto invalid;
    /* Only the selected chip in the 128-byte VGM 1.51 header; commands are
     * independently whitelisted below. 0x7a bit 0 is the emulator-only
     * AY8910 legacy-output hint for the YM2203 SSG; a real chip ignores it. */
    for (i=0x0c;i<128;++i)
        if (!((i>=0x14&&i<0x24)||(i>=0x34&&i<0x38)||(i>=0x44&&i<0x48)||
              (i==0x7a&&image[i]==1)) && image[i])
            goto invalid;
    gd3=le32(0x14);
    data_end=size;
    if (gd3) {
        /* The tag must start after at least the end command. */
        if (gd3>size-0x14-12 || gd3+0x14<first+1) goto invalid;
        data_end=gd3+0x14;
    }
    loop_pos=le32(0x1c);
    if (loop_pos) {
        if (loop_pos<first-0x1c||loop_pos>=data_end-0x1c) goto invalid;
        loop_pos+=0x1c;
    }
    loop_len=le32(0x20);
    expect=le32(0x18);
    if (!expect || expect>MAX_SAMPLES ||
        signal(SIGINT,on_signal)==SIG_ERR || signal(SIGTERM,on_signal)==SIG_ERR ||
        (gd3 && gd3_check(fd,data_end,size)) ||
        walk(fd,first,data_end,expect,0) ||
        (argc==4 && overlay_load(argv[3],expect))) goto invalid;
    /* Cue files are authored for one pass, so those tracks stay one-shot.
     * A loop length that disagrees with the walked score also plays once,
     * as every loop did before. */
    looping=loop_pos && argc==3 && loop_len==expect-loop_samples;
    if (check) {
        close(fd);
        puts(looping ? "Valid 26/K VGM; loops" : "Valid 26/K VGM");
        return 0;
    }

    if (stopped) { close(fd); return 3; }
    if (ready()) { fputs("No ready YM2203; no writes\n",stderr); close(fd); return 1; }
    if (argc==4 && overlay_start()) {
        fputs("PC-98 beeper not idle; no writes\n",stderr); close(fd); return 1;
    }
    if (clock==4000000UL)
        puts("4.000 MHz score on 3.9936 MHz 26/K: pitch 0.16% lower; timing unchanged.");
    pass_total=expect;
    status_init();
    if (gettimeofday(&start,0)) { close(fd); return 1; }
    result=walk(fd,first,data_end,expect,1);
    close(fd);
    if ((overlay_stop() | cleanup())) { fputs("Sound cleanup could not complete\n",stderr); return 1; }
    if (result) { fputs(stopped ? "Playback stopped; muted\n" : "Playback failed; muted\n",stderr); return stopped ? 3 : 1; }
    puts("Playback complete; FM and SSG muted");
    return 0;
invalid:
    close(fd);
    if (stopped) return 3;
    fputs("Unsupported or truncated 26/K VGM; no hardware writes\n",stderr);
    return 2;
}
