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

static unsigned char image[1024];
static unsigned int cur, have;
static unsigned char savedmix, touchssg;
static unsigned long pos;
static volatile unsigned char stopped;
static unsigned char touched;
static struct timeval start;
static unsigned long loop_pos;

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

static int mixer_read(unsigned char *value)
{
    if (ready()) return -1;
    asm volatile ("outb %0,%w1" :: "a" ((unsigned char)7), "d" (FM_ADDR));
    if (ready()) return -1;
    asm volatile ("inb %w1,%0" : "=a" (*value) : "d" (FM_DATA));
    return 0;
}

static int fm_reg(unsigned char r, unsigned char v)
{
    if (r == 0x28)
        return !(v & 0x0c) && (v & 3) != 3;
    if (r <= 7)
        return (r != 1 && r != 3 && r != 5) || v <= 15;
    if (r >= 8 && r <= 10) return v <= 15;
    if ((r & 3) == 3)
        return 0;
    return (r >= 0x30 && r <= 0x9e) ||
           (r >= 0xa0 && r <= 0xa6) ||
           (r >= 0xb0 && r <= 0xb2);
}

#include "overlay.inc"

static int until(unsigned long samples)
{
    struct timeval now;
    long secs, elapsed, deadline, next;
    deadline = (long)((samples / 441) * 10000UL +
                      ((samples % 441) * 10000UL + 440UL) / 441);
    while (!stopped) {
        if (gettimeofday(&now, 0))
            return -1;
        secs = now.tv_sec - start.tv_sec;
        if (secs < 0 || secs > 302)
            return -1;
        elapsed = secs * 1000000L + now.tv_usec - start.tv_usec;
        if (elapsed < 0)
            return -1;
        next=overlay_step(elapsed);
        if (elapsed >= deadline)
            return 0;
        if (next<deadline) next-=elapsed;
        else next=deadline-elapsed;
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
        if (pos==loop_pos) loopseen=1;
        cmd=getbyte(fd,end);
        if (cmd==0x66) return pos==end && samples==expect && writes && loopseen ? 0 : -1;
        if (cmd==0x55) {
            r=getbyte(fd,end); v=getbyte(fd,end);
            if (r<0 || v<0 || !fm_reg(r,v)) return -1;
            ++writes;
            if (play) {
                if (r<=10) {
                    if (!mixready && mixer_read(&savedmix)) return -1;
                    mixready=touchssg=1;
                    if (r==7) v=(v&0x3f)|(savedmix&0xc0);
                } else touched|=1<<(r==0x28?(v&3):(r&3));
                if (put(r,v)) return -1;
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
        if (play && until(samples)) return -1;
    }
    return -1;
}

static int cleanup(void)
{
    unsigned int ch, op;
    int error=0;
    for (ch=0;ch<3;++ch)
        if (touched&(1<<ch)) {
            if (put(0x28,ch)) error=1;
            for (op=0;op<4;++op)
                if (put(0x40+4*op+ch,0x7f)) error=1;
        }
    if (touchssg) {
        for (ch=8;ch<=10;++ch)
            if (put(ch,0)) error=1;
        if (put(7,savedmix)) error=1;
    }
    return error;
}

int main(int argc, char **argv)
{
    struct utsname os;
    unsigned long size, first, expect, clock, i, version;
    long len;
    int fd, n=0, got, result;
    if ((argc!=3 && argc!=4) || strcmp(argv[1],"--pc98-26k")) {
        fputs("Usage: 26kplay --pc98-26k FILE.VGM [FILE.CUE] (idle reset-default 26/K)\n",stderr);
        return 2;
    }
    if (uname(&os)||strcmp(os.machine,"pc-98 i8086")||!real_mode()) {
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
    if (n!=128||memcmp(image,"Vgm ",4)||le32(4)!=size-4||le32(0x14)!=0)
        goto invalid;
    version=le32(8);
    first=le32(0x34);
    if (version<0x151 || version>0x171 || first!=0x4c) goto invalid;
    first+=0x34;
    clock=le32(0x44);
    if (clock!=3993600UL && clock!=4000000UL) goto invalid;
    /* Only the selected chip in the 128-byte VGM 1.51 header; commands are
     * independently whitelisted below. */
    for (i=0x0c;i<128;++i)
        if (!((i>=0x18&&i<0x24)||(i>=0x34&&i<0x38)||(i>=0x44&&i<0x48)) && image[i])
            goto invalid;
    loop_pos=le32(0x1c);
    if (loop_pos) {
        if (loop_pos<first-0x1c||loop_pos>=size-0x1c) goto invalid;
        loop_pos+=0x1c;
    }
    expect=le32(0x18);
    if (!expect || expect>MAX_SAMPLES ||
        signal(SIGINT,on_signal)==SIG_ERR || signal(SIGTERM,on_signal)==SIG_ERR ||
        walk(fd,first,size,expect,0) ||
        (argc==4 && overlay_load(argv[3],expect))) goto invalid;

    if (stopped) { close(fd); return 3; }
    if (ready()) { fputs("No ready YM2203; no writes\n",stderr); close(fd); return 1; }
    if (argc==4 && overlay_start()) {
        fputs("PC-98 beeper not idle; no writes\n",stderr); close(fd); return 1;
    }
    if (clock==4000000UL)
        puts("4.000 MHz score on 3.9936 MHz 26/K: pitch 0.16% lower; timing unchanged.");
    if (gettimeofday(&start,0)) { close(fd); return 1; }
    result=walk(fd,first,size,expect,1);
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
