/* End-of-track fade shared by the direct player and the streaming
 * helpers. Each chip keeps a shadow of the levels the score set; a fade
 * level f (0 = none, FADE_STEPS = silent) adds attenuation to those
 * levels and to every later level write, so a fading song keeps playing
 * its notes while getting quieter.
 *
 * PSG (SN76489): +f attenuation steps of 2 dB on each channel. A level
 *   rewrite moves the chip's latch, so the song's own latch is restored
 *   afterwards from the shadowed tone period (same value, no audible step).
 * YM2203: FM carriers (by algorithm) get +4f total-level steps of 0.75 dB;
 *   SSG fixed levels drop by f. A hardware-envelope SSG voice cannot be
 *   scaled and is cut when the fade passes half way.
 * Define FADE_PSG and/or FADE_OPN before including. */
#ifndef FADE_STEPS
#define FADE_STEPS 16
#endif

static unsigned char fade_level;

#ifdef FADE_PSG
/* ---- SN76489 family ---- */
static unsigned char psgf_att[4] = {15, 15, 15, 15};
static unsigned char psgf_seen;     /* channels whose level the score set */
static unsigned int psgf_period[3];
static signed char psgf_latch = -1;

static unsigned char psgf_scaled(unsigned char att)
{
    unsigned int a = att + fade_level;
    return (unsigned char)(a > 15 ? 15 : a);
}

/* Filter one PSG byte the score writes: track state, return the byte to
 * send (volume latches carry the fade). */
static unsigned char psgf_byte(unsigned char v)
{
    if (v & 0x80) {
        psgf_latch = (signed char)((v >> 4) & 7);
        if (psgf_latch & 1) {
            psgf_att[psgf_latch >> 1] = v & 15;
            psgf_seen |= 1 << (psgf_latch >> 1);
            return (unsigned char)((v & 0xf0) | psgf_scaled(v & 15));
        }
        if (psgf_latch < 6)
            psgf_period[psgf_latch >> 1] = (psgf_period[psgf_latch >> 1] & 0x3f0) | (v & 15);
    } else if (psgf_latch >= 0 && psgf_latch < 6 && !(psgf_latch & 1))
        psgf_period[psgf_latch >> 1] = (psgf_period[psgf_latch >> 1] & 15) | ((v & 0x3f) << 4);
    return v;
}

/* Apply a new fade level now; write() sends one PSG byte. */
static void psgf_apply(unsigned char level, void (*write)(unsigned char))
{
    unsigned int ch;
    if (level == fade_level)
        return;
    fade_level = level;
    for (ch = 0; ch < 4; ++ch)
        if (psgf_seen & (1 << ch))
            write((unsigned char)(0x90 | (ch << 5) | psgf_scaled(psgf_att[ch])));
    /* Put the chip's latch back where the score left it. */
    if (psgf_latch >= 0 && psgf_latch < 6 && !(psgf_latch & 1))
        write((unsigned char)(0x80 | (psgf_latch << 4) |
                              (psgf_period[psgf_latch >> 1] & 15)));
    else if (psgf_latch >= 0 && (psgf_latch & 1))
        write((unsigned char)(0x80 | (psgf_latch << 4) |
                              psgf_scaled(psgf_att[psgf_latch >> 1])));
}

#endif

#ifdef FADE_OPN
/* ---- YM2203 ---- */
static unsigned char opnf_tl[3][4];   /* reset default TL is 0 */
static unsigned char opnf_alg[3];
static unsigned char opnf_ssg[3];
static unsigned char opnf_seen;     /* FM channels (bits 0-2), SSG (3-5) */

/* Carrier operators per algorithm, as register slots 40h/44h/48h/4Ch
 * (S1, S3, S2, S4). */
static unsigned char opnf_carriers(unsigned char alg)
{
    return alg <= 3 ? 0x8 : alg == 4 ? 0xc : alg <= 6 ? 0xe : 0xf;
}

static unsigned char opnf_tl_scaled(unsigned int ch, unsigned int slot)
{
    unsigned int tl = opnf_tl[ch][slot];
    if (opnf_carriers(opnf_alg[ch]) & (1 << slot)) {
        tl += 4U * fade_level;
        if (tl > 0x7f)
            tl = 0x7f;
    }
    return (unsigned char)tl;
}

static unsigned char opnf_ssg_scaled(unsigned int ch)
{
    unsigned char v = opnf_ssg[ch];
    if (v & 0x10)
        return fade_level >= FADE_STEPS / 2 ? 0 : v;
    return v > fade_level ? (unsigned char)(v - fade_level) : 0;
}

/* Filter one register write: track state, return the value to send.
 * *more is set when other registers must be rewritten (algorithm change). */
static unsigned char opnf_write(unsigned char r, unsigned char v, int *more)
{
    *more = 0;
    if (r >= 0x40 && r <= 0x4e && (r & 3) != 3) {
        opnf_tl[r & 3][(r - 0x40) >> 2] = v & 0x7f;
        opnf_seen |= 1 << (r & 3);
        return opnf_tl_scaled(r & 3, (r - 0x40) >> 2);
    }
    if (r >= 0xb0 && r <= 0xb2) {
        if ((v & 7) != opnf_alg[r - 0xb0] && fade_level)
            *more = 1;
        opnf_alg[r - 0xb0] = v & 7;
        opnf_seen |= 1 << (r - 0xb0);
    } else if (r >= 8 && r <= 10) {
        opnf_ssg[r - 8] = v;
        opnf_seen |= 8 << (r - 8);
        return opnf_ssg_scaled(r - 8);
    }
    return v;
}

/* Rewrite every carrier TL (or one channel's, ch < 3) and the SSG levels
 * for the current fade; put() sends one register write. */
static int opnf_refresh(int only_ch, int (*put)(unsigned char, unsigned char))
{
    unsigned int ch, slot;
    int err = 0;
    for (ch = 0; ch < 3; ++ch) {
        if ((only_ch >= 0 && (int)ch != only_ch) || !(opnf_seen & (1 << ch)))
            continue;
        for (slot = 0; slot < 4; ++slot)
            err |= put((unsigned char)(0x40 + 4 * slot + ch), opnf_tl_scaled(ch, slot));
    }
    if (only_ch < 0)
        for (ch = 0; ch < 3; ++ch)
            if (opnf_seen & (8 << ch))
                err |= put((unsigned char)(8 + ch), opnf_ssg_scaled(ch));
    return err;
}
#endif

/* Forget the previous track's shadows (the direct player reuses them). */
static void fade_reset(void)
{
    fade_level = 0;
#ifdef FADE_PSG
    memset(psgf_att, 15, sizeof(psgf_att));
    memset(psgf_period, 0, sizeof(psgf_period));
    psgf_latch = -1;
    psgf_seen = 0;
#endif
#ifdef FADE_OPN
    memset(opnf_tl, 0, sizeof(opnf_tl));
    memset(opnf_alg, 0, sizeof(opnf_alg));
    memset(opnf_ssg, 0, sizeof(opnf_ssg));
    opnf_seen = 0;
#endif
}
