/* Sub-tick clock for userspace on real-mode ELKS, after the kernel's own
 * get_ptime(): the kernel's jiffies (mapped once through /dev/kmem) plus
 * the count of 8254 channel 0, which ELKS runs in mode 2 at 100 Hz.
 * The pair is read with interrupts off. When /dev/kmem or its ioctls are
 * missing, ptime_init fails and callers keep tick-resolution timing.
 *
 * IBM PC: ports 43h/40h, divisor 11932 (1.1931818 MHz).
 * PC-98:  ports 77h/71h, divisor 24576 (2.4576 MHz) or 19968 (1.9968 MHz)
 *         when BIOS byte 0:501h bit 7 marks an 8 MHz-family machine. */
#include <linuxmt/mem.h>
#include <sys/ioctl.h>

static volatile unsigned long __far *pt_jiffies;
static unsigned int pt_cmd, pt_data, pt_div;
static unsigned long pt_j0;
static long pt_off, pt_last;

static int ptime_init(int pc98)
{
    int fd, kds, off;
    fd = open("/dev/kmem", O_RDONLY);
    if (fd < 0)
        return -1;
    if (ioctl(fd, MEM_GETDS, &kds) < 0 || ioctl(fd, MEM_GETJIFFADDR, &off) < 0) {
        close(fd);
        return -1;
    }
    close(fd);
    pt_jiffies = (volatile unsigned long __far *)
        (((unsigned long)(unsigned int)kds << 16) | (unsigned int)off);
    if (pc98) {
        const unsigned char __far *bios = (const unsigned char __far *)0x00000501UL;
        pt_cmd = 0x77;
        pt_data = 0x71;
        pt_div = (*bios & 0x80) ? 19968U : 24576U;
    } else {
        pt_cmd = 0x43;
        pt_data = 0x40;
        pt_div = 11932U;
    }
    return 0;
}

/* Raw snapshot: jiffies and the channel 0 count, interrupts off.
 * User code always runs with interrupts on, so cli/sti is enough. */
static unsigned int pt_snap(unsigned long *j)
{
    unsigned int lo, hi;
    asm volatile ("cli" ::: "memory");
    asm volatile ("outb %%al,%%dx" :: "a" (0U), "d" (pt_cmd) : "memory");
    *j = *pt_jiffies;
    asm volatile ("inb %%dx,%%al" : "=a" (lo) : "d" (pt_data) : "memory");
    asm volatile ("inb %%dx,%%al" : "=a" (hi) : "d" (pt_data) : "memory");
    asm volatile ("sti" ::: "memory");
    lo = (lo & 0xff) | ((hi & 0xff) << 8);
    return lo > pt_div ? pt_div : lo;
}

/* Raw time in us since jiffy pt_j0, made monotonic: a counter that
 * reloaded before its tick interrupt ran reads as a step back of nearly a
 * tick, which is one tick the kernel has not counted yet. */
static long pt_raw(void)
{
    unsigned long j;
    unsigned int c = pt_snap(&j);
    long us = (long)(j - pt_j0) * 10000L +
              (long)((unsigned long)(pt_div - c) * 10000UL / pt_div);
    if (us < pt_last && pt_last - us < 10000L)
        us += 10000L;
    pt_last = us;
    return us;
}

/* Start the clock at zero now. */
static void ptime_start(void)
{
    pt_snap(&pt_j0);
    pt_last = 0;
    pt_off = pt_raw();
}

/* Microseconds since ptime_start. */
static long ptime_us(void)
{
    return pt_raw() - pt_off;
}
