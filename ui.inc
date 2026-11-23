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
static unsigned char last_tick, paint_state[6];
static unsigned char meters[6], meter_known;
static unsigned long next_key_poll;

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

static void update_meters(int force)
{
    static const char *const fm_labels[] = {"FM 1", "FM 2", "FM 3", "SSG A", "SSG B", "SSG C"};
    static const char *const psg_labels[] = {"TONE 1", "TONE 2", "TONE 3", "NOISE"};
    unsigned int i, value, packed;
    int bars;
    if (pcm || opl)
        return;
    for (i = 0; i < (psg ? 4 : ssg ? 6 : 3); ++i) {
        value = meters[i];
        packed = value | ((meter_known & (1 << i)) ? 0x80 : 0);
        if (!force && paint_state[i] == packed)
            continue;
        paint_state[i] = packed;
        bars = psg || i >= 3 ? (value * 25 + 7) / 15 : (value * 25) / 4;
        cursor((ssg ? 15 : 16)+i, 4);
        fputs(C_AQUA, stdout);
        field(psg ? psg_labels[i] : fm_labels[i], 8);
        fputs(C_RESET, stdout);
        if (packed & 0x80)
            printf("%2u   ", value);
        else
            fputs(" -   ", stdout);
        putchar('[');
        fputs(C_LIME, stdout); chars('#', bars);
        fputs(C_BLUE, stdout); chars('.', 25-bars);
        fputs(C_RESET "]", stdout);
    }
}

static void update_clock(unsigned long position)
{
    unsigned int fill = 0;
    if (total && position <= total)
        fill = (unsigned int)(position * 31UL / total);
    cursor(8, 4);
    fputs(C_GOLD, stdout);
    if (pcm || opl)
        fputs("--:--.-", stdout);
    else
        timecode(position);
    fputs(C_RESET " / ", stdout);
    if (total)
        timecode(total);
    else
        fputs("--:--.-", stdout);
    cursor(9, 4);
    fputs(C_BLUE "[", stdout);
    fputs(C_LIME, stdout); chars('=', fill);
    fputs(C_BLUE, stdout); chars('.', 31-fill);
    fputs("]" C_RESET, stdout);
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
    frame(ssg ? 13 : 14, 21, 2, 48);
    label(5, 3, 46, " NOW PLAYING");
    label(5, 52, 26, " PLAY QUEUE");
    label(ssg ? 14 : 15, 3, 46, pcm ? " SOUND BLASTER / DIGITAL" : opl ? " ADLIB / MELODIC FM" : psg ? " SIGNAL / PSG VOLUME 0-15" : ssg ? " SIGNAL  /  FM KEYS 0-4  /  SSG VOL 0-15" : " SIGNAL / FM KEYED OPERATORS 0-4");
    cursor(7, 4);
    fputs(C_RESET, stdout); field(title[0] ? title : base(files[selected]), 44);
    update_clock(shown);
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
        field(base(files[j]), 21);
        fputs(C_RESET, stdout);
    }
    cursor(20, 53);
    printf("%02d / %02d  files", selected+1, count);
    if (pcm || opl) {
        cursor(17, 4);
        fputs(C_AQUA, stdout);
        fputs(pcm ? "PCM WAV / 8-bit mono / 4-20 kHz" : "YM3812 / 9-channel melodic VGM", stdout);
        fputs(C_RESET, stdout);
        cursor(19, 4);
        fputs("Live meters and clock unavailable.", stdout);
    } else {
        update_meters(1);
        if (!ssg) {
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

int deck_pump(unsigned long position_samples)
{
    unsigned char tick = (unsigned char)(position_samples / 11025UL);
    if (position_samples >= next_key_poll) {
        input();
        next_key_poll = position_samples + 882UL;
    }
    if (!stopped && tick != last_tick) {
        shown = position_samples;
        last_tick = tick;
        update_clock(shown);
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
