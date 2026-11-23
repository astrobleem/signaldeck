/* Status sender shared by the streaming helpers; see status.h. */
#include <sys/stat.h>
#include "status.h"

static unsigned char st_on, st_known, st_env;
static unsigned char st_cur[STATUS_CHANNELS], st_hi[STATUS_CHANNELS];
static long st_last = -1000000L;

/* Only a pipe on fd 3 counts, so a standalone run never writes there. */
static void status_init(void)
{
    struct stat s;
    int flags;
    if (fstat(STATUS_FD, &s) || !S_ISFIFO(s.st_mode))
        return;
    flags = fcntl(STATUS_FD, F_GETFL);
    if (flags < 0 || fcntl(STATUS_FD, F_SETFL, flags | O_NONBLOCK) < 0)
        return;
    signal(SIGPIPE, SIG_IGN);
    st_on = 1;
}

static void status_level(unsigned int ch, unsigned char level, int env)
{
    st_cur[ch] = level;
    if (level > st_hi[ch])
        st_hi[ch] = level;
    st_known |= 1 << ch;
    if (env)
        st_env |= 1 << ch;
    else
        st_env &= ~(1 << ch);
}

static void put32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

/* Returns 1 when a record was due, so the caller rereads the clock. */
static int status_due(long elapsed, unsigned long position, unsigned long total)
{
    unsigned char b[STATUS_SIZE];
    if (!st_on || elapsed - st_last < 100000L)
        return 0;
    st_last = elapsed;
    b[0] = 'S';
    b[1] = 'D';
    b[2] = st_known;
    b[3] = st_env;
    put32(b + 4, position > total ? total : position);
    put32(b + 8, total);
    memcpy(b + 12, st_cur, STATUS_CHANNELS);
    memcpy(b + 18, st_hi, STATUS_CHANNELS);
    if (write(STATUS_FD, b, STATUS_SIZE) == STATUS_SIZE)
        memcpy(st_hi, st_cur, STATUS_CHANNELS);
    return 1;
}
