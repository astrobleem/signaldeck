/* Native ELKS console deck. Plain ASCII cells for both machines. */
#define C_RESET "\033[0m"
#define C_AQUA "\033[36m"
#define C_PINK "\033[35m"
#define C_BLUE "\033[34m"
#define C_LIME "\033[32m"
#define C_GOLD "\033[33m"
#define C_TAG "\033[46m"
#define C_HOT "\033[42m"

static struct termios saved_tty;
static int saved_flags, terminal;
static int ui, active, action, selected, count;
static char **files;
static const char *chip;
static const char *note;
static char title[45];
static unsigned long total, shown;
static unsigned long last_tick;
static unsigned char meters[6], meter_known, meter_env;
static unsigned long next_key_poll;

/* Meters repaint at 10 Hz and only the cells that differ from what is on
 * screen. meter_hi latches the highest level written since the last
 * repaint, so a key-on or volume blip between repaints still reaches the
 * peak marker, which holds for PEAK_HOLD repaints and then falls. */
#define BAR_CELLS 25
#define PEAK_HOLD 5                 /* 0.5 s */
#define PEAK_FALL 5U                /* cells per repaint after the hold */
#define CLOCK_CELLS 31
static unsigned char meter_hi[6], peak[6], peak_age[6];
static unsigned char cell_shown[6][BAR_CELLS];  /* 0 = not on screen */
static unsigned char value_shown[6];
static unsigned char clock_fill_shown;
static unsigned int clock_dec_shown;
static const char *ink;

static void meter_level(unsigned int i, unsigned char v)
{
    meters[i] = v;
    if (v > meter_hi[i])
        meter_hi[i] = v;
}

static void meters_clear(void)
{
    meter_known = meter_env = 0;
    memset(meters, 0, sizeof(meters));
    memset(meter_hi, 0, sizeof(meter_hi));
    memset(peak, 0, sizeof(peak));
    memset(peak_age, 0, sizeof(peak_age));
}

static void cursor(int y, int x)
{
    printf("\033[%d;%dH", y, x);
}

/* Never let a filename or tag become an escape sequence. */
static const char *base(const char *p)
{
    const char *q = strrchr(p, '/');
    return q ? q + 1 : p;
}

static void field(const char *s, int width)
{
    unsigned char c;
    while (width-- > 0) {
        c = *s ? *s++ : ' ';
        putchar(c >= 32 && c < 127 ? c : '?');
    }
}

static void chars(int c, int n)
{
    while (n-- > 0)
        putchar(c);
}

static void label(int y, int x, int width, const char *s)
{
    cursor(y, x);
    fputs(C_RESET C_TAG, stdout);
    field(s, width);
    fputs(C_RESET, stdout);
}

static void frame(int y, int bottom, int x, int w)
{
    int i;
    fputs(C_BLUE, stdout);
    cursor(y, x);
    putchar('+'); chars('-', w - 2); putchar('+');
    for (i = y+1; i < bottom; ++i) {
        cursor(i, x); putchar('|');
        cursor(i, x+w-1); putchar('|');
    }
    cursor(bottom, x);
    putchar('+'); chars('-', w - 2); putchar('+');
    fputs(C_RESET, stdout);
}

static void timecode(unsigned long t)
{
    unsigned int dec = (unsigned int)(t / 4410UL);
    printf("%02u:%02u.%u", dec / 600, (dec / 10) % 60, dec % 10);
}

static void paint(const char *colour, int c)
{
    if (ink != colour) {
        fputs(colour, stdout);
        ink = colour;
    }
    putchar(c);
}

/* Six rows (FM + SSG) for the 26K FM+SSG and stream modes. */
static int wide(void)
{
    return ssg || pcstream;
}

/* Four PSG rows (3 tones + noise) for the Tandy direct and stream modes. */
static int psg_rows(void)
{
    return psg || tstream;
}

static unsigned int meter_cells(unsigned int i, unsigned int value)
{
    return psg_rows() || i >= 3 ? (value * BAR_CELLS + 7) / 15 : (value * BAR_CELLS) / 4;
}

static unsigned int meter_count(void)
{
    return psg_rows() ? 4 : wide() ? 6 : 3;
}

/* Called once per 10 Hz repaint, before drawing. */
static void meter_tick(void)
{
    unsigned int i, level, hi;
    for (i = 0; i < meter_count(); ++i) {
        level = meter_cells(i, meters[i]);
        hi = meter_cells(i, meter_hi[i]);
        meter_hi[i] = meters[i];
        if (hi >= peak[i]) {
            peak[i] = hi;
            peak_age[i] = 0;
        } else if (peak_age[i] < PEAK_HOLD)
            ++peak_age[i];
        else
            peak[i] = (unsigned char)(peak[i] > level + PEAK_FALL ?
                                      peak[i] - PEAK_FALL : level);
    }
}

static void update_meter_rows(int force, unsigned int from, unsigned int to);

static void update_meters(int force)
{
    update_meter_rows(force, 0, 6);
}

/* Repaint rows from..to-1 only (the stream view spreads a repaint over
 * several short slices so the helper is never kept waiting long). */
static void update_meter_rows(int force, unsigned int from, unsigned int to)
{
    static const char *const fm_labels[] = {"FM 1", "FM 2", "FM 3", "SSG A", "SSG B", "SSG C"};
    static const char *const psg_labels[] = {"TONE 1", "TONE 2", "TONE 3", "NOISE"};
    unsigned int i, c, value, packed, level, y, next;
    unsigned char want;
    if (pcm || opl)
        return;
    ink = 0;
    if (to > meter_count())
        to = meter_count();
    for (i = from; i < to; ++i) {
        y = (wide() ? 15 : 16) + i;
        value = meters[i];
        packed = value | ((meter_known & (1 << i)) ? 0x80 : 0) |
                 ((meter_env & (1 << i)) ? 0x40 : 0);
        if (force) {
            cursor(y, 4);
            fputs(C_AQUA, stdout);
            field(psg_rows() ? psg_labels[i] : fm_labels[i], 8);
            cursor(y, 17);
            fputs(C_RESET "[", stdout);
            cursor(y, 18 + BAR_CELLS);
            putchar(']');
            ink = C_RESET;
            memset(cell_shown[i], 0, BAR_CELLS);
            value_shown[i] = 0xff;
        }
        if (value_shown[i] != packed) {
            cursor(y, 12);
            fputs(C_RESET, stdout);
            ink = C_RESET;
            if ((packed & 0xc0) == 0xc0)
                fputs(" E", stdout);       /* SSG hardware envelope */
            else if (packed & 0x80)
                printf("%2u", value);
            else
                fputs(" -", stdout);
            value_shown[i] = packed;
        }
        level = meter_cells(i, value);
        next = BAR_CELLS + 1;
        for (c = 0; c < BAR_CELLS; ++c) {
            want = c < level ? '#' : (peak[i] > level && c + 1 == peak[i]) ? '|' : '.';
            if (cell_shown[i][c] == want)
                continue;
            if (c != next)
                cursor(y, 18 + c);
            paint(want == '#' ? C_LIME : want == '|' ? C_GOLD : C_BLUE, want);
            cell_shown[i][c] = want;
            next = c + 1;
        }
    }
    fputs(C_RESET, stdout);
    ink = 0;
}

static void update_clock(unsigned long position, int force)
{
    unsigned int c, n, fill = 0, dec = (unsigned int)(position / 4410UL);
    if (total && position <= total)
        fill = (unsigned int)(position * (unsigned long)CLOCK_CELLS / total);
    if (force || dec != clock_dec_shown) {
        cursor(8, 4);
        fputs(C_GOLD, stdout);
        if (pcm || opl || (STREAMING && !total))
            fputs("--:--.-", stdout);
        else
            timecode(position);
        fputs(C_RESET, stdout);
        clock_dec_shown = dec;
    }
    if (force) {
        fputs(" / ", stdout);
        if (total)
            timecode(total);
        else
            fputs("--:--.-", stdout);
        cursor(9, 4);
        fputs(C_BLUE "[", stdout);
        fputs(C_LIME, stdout); chars('=', fill);
        fputs(C_BLUE, stdout); chars('.', CLOCK_CELLS-fill);
        fputs("]" C_RESET, stdout);
    } else if (fill != clock_fill_shown) {
        c = fill < clock_fill_shown ? fill : clock_fill_shown;
        n = fill < clock_fill_shown ? clock_fill_shown - fill : fill - clock_fill_shown;
        cursor(9, 5 + c);
        fputs(fill > clock_fill_shown ? C_LIME : C_BLUE, stdout);
        chars(fill > clock_fill_shown ? '=' : '.', n);
        fputs(C_RESET, stdout);
    }
    clock_fill_shown = fill;
}

static void dashboard(void)
{
    int i, j, first = selected > 5 ? selected-5 : 0;
    fputs(C_RESET "\033[2J\033[?25l", stdout);
    cursor(2, 3);
    fputs(C_PINK "SIGNAL" C_RESET " / " C_AQUA "DECK" C_RESET, stdout);
    cursor(2, 57);
    fputs(C_AQUA, stdout); field(chip, 21); fputs(C_RESET, stdout);
    frame(4, 12, 2, 48);
    frame(4, 21, 51, 28);
    frame(wide() ? 13 : 14, 21, 2, 48);
    label(5, 3, 46, " NOW PLAYING");
    label(5, 52, 26, " PLAY QUEUE");
    label(wide() ? 14 : 15, 3, 46, pcm ? " SOUND BLASTER / DIGITAL" : opl ? " ADLIB / MELODIC FM" :
          pcstream ? (cue_files[selected] ? " STREAM+CUES / FM KEYS 0-4 / SSG VOL 0-15" :
                                           " STREAM  /  FM KEYS 0-4  /  SSG VOL 0-15") : tstream ? " STREAM / PSG VOLUME 0-15" : psg ? " SIGNAL / PSG VOLUME 0-15" : ssg ? " SIGNAL  /  FM KEYS 0-4  /  SSG VOL 0-15" : " SIGNAL / FM KEYED OPERATORS 0-4");
    cursor(7, 4);
    fputs(C_RESET, stdout); field(title[0] ? title : base(files[selected]), 44);
    update_clock(shown, 1);
    cursor(11, 4);
    fputs(active ? C_HOT : C_TAG, stdout);
    field(active ? " > PLAYING " : " * STOPPED ", 12);
    fputs(C_RESET " ", stdout);
    field(note, 30);
    for (i = 0; i < 12 && first+i < count; ++i) {
        j = first+i;
        cursor(7+i, 52);
        fputs(j == selected ? (active ? C_HOT : C_TAG) : C_AQUA, stdout);
        printf(" %c%02d ", j == selected ? (active ? '>' : '*') : ' ', j+1);
        field(base(files[j]), pcstream ? 19 : 21);
        if (pcstream)
            fputs(cue_files[j] ? " +" : "  ", stdout);
        fputs(C_RESET, stdout);
    }
    cursor(20, 53);
    printf("%02d / %02d  files", selected+1, count);
    if (pcm || opl) {
        cursor(17, 4);
        fputs(C_AQUA, stdout);
        fputs(pcm ? "PCM WAV / 8-bit mono / 4-20 kHz" :
              "YM3812 / 9-channel melodic VGM", stdout);
        fputs(C_RESET, stdout);
        cursor(19, 4);
        fputs("Live meters and clock unavailable.", stdout);
    } else {
        update_meters(1);
        if (!wide()) {
            cursor(20, 4);
            fputs(C_AQUA "Activity from chip writes, not audio level." C_RESET, stdout);
        } else {
            cursor(24, 2);
            fputs(C_AQUA "Activity from chip writes, not audio level." C_RESET, stdout);
        }
    }
    cursor(23, 2);
    fputs(C_TAG " ENTER " C_RESET " Play   " C_TAG " S " C_RESET " Stop   "
          C_TAG " P/N " C_RESET " Prev/Next   " C_TAG " Q " C_RESET " Quit", stdout);
    fflush(stdout);
}

static void input(void)
{
    char c;
    while (read(0, &c, 1) == 1) {
        if (c == 'q' || c == 'Q') {
            action = 'q'; stopped = 1; return;
        }
        if (c == 's' || c == 'S') {
            action = 's'; stopped = 1; return;
        }
        if (c == 'p' || c == 'P' || c == 'n' || c == 'N') {
            action = c | 32; stopped = 1; return;
        }
        if (!active && (c == '\r' || c == '\n' || c == ' ')) {
            action = ' '; return;
        }
    }
}

/* Called from the player only while it has at least one tick of slack. */
int deck_pump(unsigned long position_samples)
{
    unsigned long tick = position_samples / 4410UL;     /* 10 Hz */
    if (position_samples >= next_key_poll) {
        input();
        next_key_poll = position_samples + 882UL;
    }
    if (!stopped && tick != last_tick) {
        shown = position_samples;
        last_tick = tick;
        meter_tick();
        update_clock(shown, 0);
        update_meters(0);
        fflush(stdout);
    }
    return stopped;
}

static int term_on(void)
{
    struct termios t;
    if (!isatty(0) || !isatty(1) || tcgetattr(0, &saved_tty))
        return -1;
    saved_flags = fcntl(0, F_GETFL);
    if (saved_flags < 0)
        return -1;
    t = saved_tty;
    t.c_lflag &= ~(ICANON | ECHO);
    if (tcsetattr(0, TCSAFLUSH, &t))
        return -1;
    if (fcntl(0, F_SETFL, saved_flags | O_NONBLOCK)) {
        tcsetattr(0, TCSAFLUSH, &saved_tty);
        return -1;
    }
    terminal = 1;
    return 0;
}

static void term_off(void)
{
    if (terminal) {
        fcntl(0, F_SETFL, saved_flags);
        tcsetattr(0, TCSAFLUSH, &saved_tty);
        fputs(C_RESET "\033[?25h\033[2J\033[H", stdout);
        fflush(stdout);
    }
}
