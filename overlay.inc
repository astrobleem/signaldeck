/* Opt-in PC-9801 PIT1 beeper overlay. P98D cue: u32 duration in
 * 44.1 kHz samples, u16 count, entries [u32 timestamp, u8 voice].
 * The score and sidecar are validated before any hardware writes.
 */
#define MAX_CUES 1024
#define NO_EVENT 0x7fffffffL
#define BEEP_MODE 0x77
#define BEEP_COUNT 0x73
#define BEEP_GATE 0x37
#define BEEP_STATE 0x35
static unsigned char cue[MAX_CUES * 5];
static unsigned int cue_count, cue_next;
static unsigned char enabled, beep_on, beep_changed, phase, voice;
static unsigned long pit_hz;
static long beat_next=NO_EVENT, beat_part=NO_EVENT, beat_end=NO_EVENT;

static void io_write(unsigned int p, unsigned char v)
{
    asm volatile("outb %0,%w1"::"a"(v),"d"(p));
}

static unsigned char io_read(unsigned int p)
{
    unsigned char v;
    asm volatile("inb %w1,%0":"=a"(v):"d"(p));
    return v;
}

static void tone(unsigned int hz)
{
    unsigned int div=(unsigned int)((pit_hz+hz/2)/hz);
    io_write(BEEP_MODE,0x76); /* counter 1 only, low/high, square wave */
    io_write(BEEP_COUNT,(unsigned char)div);
    io_write(BEEP_COUNT,(unsigned char)(div>>8));
}

static long due(unsigned long n)
{
    return (long)((n/441)*10000UL+((n%441)*10000UL+440UL)/441);
}

static unsigned long cue_time(unsigned int idx)
{
    const unsigned char *p=cue+5*idx;
    return (unsigned long)p[0] | ((unsigned long)p[1]<<8) |
           ((unsigned long)p[2]<<16) | ((unsigned long)p[3]<<24);
}

static int cue_read(int f, unsigned char *p, unsigned int n)
{
    int got;
    while (n && !stopped) {
        got=read(f,p,n);
        if (got<=0) return -1;
        p+=got; n-=got;
    }
    return n?-1:0;
}

static int overlay_load(const char *path, unsigned long duration)
{
    unsigned int i;
    unsigned long at, prev=0;
    int f=open(path,O_RDONLY);
    if (f<0) return -1;
    if (cue_read(f,image,10) || memcmp(image,"P98D",4) || le32(4)!=duration)
        goto invalid;
    cue_count=image[8]|((unsigned int)image[9]<<8);
    if (!cue_count || cue_count>MAX_CUES ||
        cue_read(f,cue,cue_count*5) || read(f,image,1)!=0)
        goto invalid;
    for (i=0;i<cue_count;++i) {
        at=cue_time(i);
        if ((i && at<=prev) || at>=duration ||
            (cue[5*i+4]!=1 && cue[5*i+4]!=2)) goto invalid;
        prev=at;
    }
    close(f);
    beat_next=due(cue_time(0));
    return 0;
invalid:
    close(f);
    return -1;
}

static int overlay_start(void)
{
    volatile const unsigned char __far *bios=(volatile const unsigned char __far *)0x00000501UL;
    unsigned char c=io_read(BEEP_STATE);
    if (c==255 || !(c&8)) return -1;
    pit_hz=(*bios&0x80)?1996800UL:2457600UL;
    enabled=1;
    return 0;
}

static void overlay_mute(void)
{
    if (beep_on) {
        io_write(BEEP_GATE,7);
        beep_on=0;
    }
}

static long overlay_step(long elapsed)
{
    long at, next;
    if (!enabled) return NO_EVENT;
    while (cue_next<cue_count && beat_next<=elapsed) {
        at=beat_next;
        voice=cue[5*cue_next+4];
        ++cue_next;
        beat_next=cue_next<cue_count?due(cue_time(cue_next)):NO_EVENT;
        beat_part=at+(voice==1?20000L:10000L);
        beat_end=at+(voice==1?40000L:20000L);
        phase=0;
    }
    if (elapsed>=beat_end) {
        overlay_mute();
        beat_part=beat_end=NO_EVENT;
    } else if (beat_end!=NO_EVENT) {
        unsigned char want=elapsed>=beat_part?2:1;
        if (phase!=want) {
            beep_changed=1;
            tone(voice==1?(want==1?144:88):(want==1?520:267));
            if (!beep_on) {
                io_write(BEEP_GATE,6);
                beep_on=1;
            }
            phase=want;
        }
    }
    next=beat_next;
    if (beep_on) {
        at=phase==1?beat_part:beat_end;
        if (at<next) next=at;
    }
    return next;
}

static int overlay_stop(void)
{
    unsigned char c;
    if (!enabled || !beep_changed) return 0;
    overlay_mute();
    tone(2000);  /* reset-default pitch; prior PIT reload is not readable */
    c=io_read(BEEP_STATE);
    return (c==255 || !(c&8))?-1:0;
}
