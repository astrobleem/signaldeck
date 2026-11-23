/* Streaming YM2203 player for an idle reset-default PC-9801-26/K on
 * real-mode PC-98 ELKS. See stream.inc for the shared streaming core. */
#define MAX_FILE (256UL * 1024UL)
#define MAX_SAMPLES (300UL * 44100UL)
#include "stream.inc"

#define FM_ADDR 0x188
#define FM_DATA 0x18a

static unsigned char savedmix, touchssg, mixready;
static unsigned char savedenv[3], envsaved;
static unsigned char touched, ssgeg, mode3;

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

static long chip_next_event(long elapsed)
{
    return overlay_step(elapsed);
}

static void chip_loop_state(int restore)
{
    (void)restore;                 /* YM2203 writes carry no parse state */
}

static unsigned char ssg_mix = 0x3f, ssg_vol[3];

/* Report what the chip was told, as the direct player does: FM keyed
 * operators per channel and SSG level per sounding voice. */
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

static int chip_command(int cmd, int fd, unsigned long end, int play)
{
    int r, v;
    if (cmd != 0x55)
        return 0;
    GETB(r); GETB(v);
    if (r<0 || v<0 || !fm_reg(r,v)) return -1;
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
    return 1;
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
    struct vgm g;
    unsigned long clock;
    unsigned int i, hdr;
    int fd, result, check;
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
    want_far = !check;    /* playback keeps a far copy when it can */
    if (signal(SIGINT,on_signal)==SIG_ERR || signal(SIGTERM,on_signal)==SIG_ERR ||
        vgm_header(fd,&g,&hdr) || g.version<0x151 || g.first!=0x80)
        goto invalid;
    clock=le32(0x44);
    if (clock!=3993600UL && clock!=4000000UL) goto invalid;
    /* Only the selected chip in the 128-byte VGM 1.51 header; commands are
     * independently whitelisted. 0x7a bit 0 is the emulator-only AY8910
     * legacy-output hint for the YM2203 SSG; a real chip ignores it. */
    for (i=0x0c;i<128;++i)
        if (!((i>=0x14&&i<0x24)||(i>=0x34&&i<0x38)||(i>=0x44&&i<0x48)||
              (i==0x7a&&image[i]==1)) && image[i])
            goto invalid;
    if (vgm_body(fd,&g) || (argc==4 && overlay_load(argv[3],g.expect)))
        goto invalid;
    /* Cue files are authored for one pass, so those tracks stay one-shot.
     * A loop length that disagrees with the walked score also plays once,
     * as every loop did before. */
    looping=argc==3 && loop_ok(&g);
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
    status_init();
    if (clock_start(1)) { close(fd); return 1; }
    result=walk(fd,&g,1);
    close(fd);
    far_release();
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
