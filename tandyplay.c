/* Streaming SN76489-family PSG player for the Tandy 1000 EX sound chip
 * at port C0h on real-mode IBM PC ELKS. The score is validated in full
 * before any hardware access; see stream.inc for the streaming core. */
#define MAX_FILE (512UL * 1024UL)
#define MAX_SAMPLES (600UL * 44100UL)
#include "stream.inc"

static unsigned char touched;
static int latched = -1;

static void psg_write(unsigned char v)
{
    /* SN76496 boards need time between back-to-back OUTs. */
    asm volatile ("outb %0,$0xc0; nop; nop; nop; nop; nop; nop;"
                  "nop; nop; nop; nop; nop; nop" :: "a" (v));
}

static int tandy_rom(void)
{
    const unsigned char __far *rom = (const unsigned char __far *)0xf0000000UL;
    /* Family fingerprint, the same one the direct player uses. */
    return rom[0xc000U] == 0x21 && rom[0xfffeU] == 0xff;
}

static long chip_next_event(long elapsed)
{
    (void)elapsed;
    return NO_EVENT;
}

/* The chip keeps its latch across the loop seam. If the first PSG byte
 * after the loop point is a data byte, its target is the latch at the end
 * of the song, so the loop replays only when that matches the latch the
 * data byte was checked against. */
static int loop_latch = -1;
static unsigned char loop_first_pending, loop_data_first;

static void chip_loop_state(int restore)
{
    if (restore) {
        latched = loop_latch;
    } else {
        loop_latch = latched;
        loop_first_pending = 1;
    }
}

/* Only 0x50 writes. A data byte must follow a tone-frequency latch; the
 * noise and volume registers take their value in the latch byte. */
static int chip_command(int cmd, int fd, unsigned long end, int play)
{
    int v;
    if (cmd != 0x50)
        return 0;
    v = getbyte(fd, end);
    if (v < 0)
        return -1;
    if (loop_first_pending && !play) {
        loop_first_pending = 0;
        loop_data_first = !(v & 0x80);
    }
    if (v & 0x80)
        latched = (v >> 4) & 7;
    else if ((v & 0x40) || latched < 0 || (latched & 1) || latched == 6)
        return -1;
    if (play) {
        touched = 1;
        psg_write((unsigned char)v);
        if ((v & 0x80) && (latched & 1) && st_on)
            status_level(latched >> 1, 15 - (v & 15), 0);
    }
    return 1;
}

static void cleanup(void)
{
    unsigned int ch;
    if (touched)
        for (ch = 0; ch < 4; ++ch)
            psg_write(0x9f + (ch << 5));
}

int main(int argc, char **argv)
{
    struct utsname os;
    struct vgm g;
    unsigned int i, hdr;
    int fd, result, check;
    check = argc > 1 && !strcmp(argv[1], "--check");
    if (argc != 3 || (!check && strcmp(argv[1], "--tandy-ex"))) {
        fputs("Usage: tandyplay --tandy-ex|--check FILE.VGM (idle Tandy 1000 EX PSG)\n", stderr);
        return 2;
    }
    if (!check && (uname(&os) || strcmp(os.machine, "ibmpc i8086") || !real_mode())) {
        fputs("Requires real-mode IBM PC ELKS; no I/O performed\n", stderr);
        return 1;
    }
    if (!check && !tandy_rom()) {
        fputs("Tandy BIOS signature missing; no I/O performed\n", stderr);
        return 1;
    }
    fd = open(argv[2], O_RDONLY);
    if (fd < 0) { perror(argv[2]); return 1; }
    if (signal(SIGINT, on_signal) == SIG_ERR || signal(SIGTERM, on_signal) == SIG_ERR ||
        vgm_header(fd, &g, &hdr))
        goto invalid;
    /* One SN76489-family chip at 3.579545 MHz with the Tandy/TI noise
     * shape (feedback 0x0006, 15-bit register, no flags). Besides the
     * chip fields, only the GD3/length/loop fields, the frame-rate hint
     * and the data offset may be set. */
    if (le32(0x0c) != 3579545UL || image[0x28] != 6 || image[0x29] ||
        image[0x2a] != 15 || image[0x2b])
        goto invalid;
    for (i = 0x10; i < hdr; ++i)
        if (!((i >= 0x14 && i < 0x28) || (i >= 0x28 && i < 0x2c) ||
              (i >= 0x34 && i < 0x38)) && image[i])
            goto invalid;
    if (vgm_body(fd, &g))
        goto invalid;
    looping = loop_ok(&g) && (!loop_data_first || latched == loop_latch);
    if (check) {
        close(fd);
        puts(looping ? "Valid Tandy PSG VGM; loops" : "Valid Tandy PSG VGM");
        return 0;
    }
    if (stopped) { close(fd); return 3; }
    status_init();
    latched = -1;
    if (gettimeofday(&start, 0)) { close(fd); return 1; }
    result = walk(fd, &g, 1);
    close(fd);
    cleanup();
    if (result) {
        fputs(stopped ? "Playback stopped; muted\n" : "Playback failed; muted\n", stderr);
        return stopped ? 3 : 1;
    }
    puts("Playback complete; PSG muted");
    return 0;
invalid:
    close(fd);
    if (stopped) return 3;
    fputs("Unsupported or truncated Tandy PSG VGM; no hardware writes\n", stderr);
    return 2;
}
