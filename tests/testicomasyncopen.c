/*
 * Opens an emulated IC-7610 with async data enabled.
 *
 * With async data enabled, replies are read from the sync data pipe that the
 * async data handler fills from the port. That handler starts only after the
 * backend's rig_open() has finished, so the transactions the backend makes
 * while opening, such as the Icom USB echo check, must read the port itself;
 * otherwise they time out and the rig cannot be opened at all.
 *
 * The rig is opened twice: once quiet, and once streaming spectrum scope data
 * unsolicited all the time, as an IC-7610 does while its scope data output is
 * on, and missing the first command it is sent. Every scope frame is a frame
 * read before the read's timeout, so a transaction that waited for its reply
 * from one frame to the next would wait for ever; it must time out instead,
 * and the rig still open.
 */

#if defined(_WIN32) || defined(WIN32)

int main(void)
{
    return 77;
}

#else

#define _XOPEN_SOURCE 600

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "hamlib/rig.h"

#define CIV_PREAMBLE 0xfe
#define CIV_END 0xfd
#define CIV_RIG_ADDRESS 0x98
#define CIV_CONTROLLER_ADDRESS 0xe0
#define CIV_OK 0xfb
#define CIV_FRAME_MAX 64

/* An IC-7610 with its scope data output on sends about 62 scope frames a second */
#define SCOPE_FRAME_INTERVAL_MS 16
#define SCOPE_FRAME_DATA_LEN 50
#define SCOPE_FRAME_SEQUENCE_MAX 0x15

/* The emulated rig opens in about two seconds, three with a command missed */
#define OPEN_TIME_LIMIT_S 20

/* 14.074000 MHz as five BCD bytes, least significant first */
static const unsigned char frequency_bcd[5] = { 0x00, 0x40, 0x07, 0x14, 0x00 };

static int expect(int condition, const char *description)
{
    printf("%s: %s\n", condition ? "PASS" : "FAIL", description);
    return condition ? 0 : 1;
}

static void reply(int fd, const unsigned char *payload, int payload_len)
{
    unsigned char frame[CIV_FRAME_MAX];
    int len = 0;

    frame[len++] = CIV_PREAMBLE;
    frame[len++] = CIV_PREAMBLE;
    frame[len++] = CIV_CONTROLLER_ADDRESS;
    frame[len++] = CIV_RIG_ADDRESS;
    memcpy(frame + len, payload, payload_len);
    len += payload_len;
    frame[len++] = CIV_END;

    if (write(fd, frame, len) != len)
    {
        _exit(2);
    }
}

/* Sends one frame of spectrum scope data (27 00), as the rig does unsolicited
 * while its scope data output is on: main scope, division sequence, the last
 * division number, then the waveform data. */
static void send_scope_frame(int fd)
{
    static unsigned char sequence = 1;
    unsigned char payload[4 + 1 + SCOPE_FRAME_DATA_LEN];
    int len = 0;

    payload[len++] = 0x27;
    payload[len++] = 0x00;
    payload[len++] = 0x00;
    payload[len++] = sequence;
    payload[len++] = SCOPE_FRAME_SEQUENCE_MAX;
    memset(payload + len, 0x10, SCOPE_FRAME_DATA_LEN);
    len += SCOPE_FRAME_DATA_LEN;
    reply(fd, payload, len);

    sequence = sequence == SCOPE_FRAME_SEQUENCE_MAX ? 1 : sequence + 1;
}

/* Answers one CI-V command an IC-7610 backend sends while opening and reading
 * the frequency, without echoing it, as a rig with USB echo off does. */
static void answer(int fd, const unsigned char *frame, int len)
{
    unsigned char payload[CIV_FRAME_MAX];
    unsigned char cmd;

    if (len < 6 || frame[2] != CIV_RIG_ADDRESS)
    {
        return;
    }

    cmd = frame[4];

    switch (cmd)
    {
    case 0x03: /* read the operating frequency */
        payload[0] = cmd;
        memcpy(payload + 1, frequency_bcd, sizeof(frequency_bcd));
        reply(fd, payload, 1 + sizeof(frequency_bcd));
        break;

    case 0x25: /* read the selected or unselected VFO's frequency */
        payload[0] = cmd;
        payload[1] = frame[5];
        memcpy(payload + 2, frequency_bcd, sizeof(frequency_bcd));
        reply(fd, payload, 2 + sizeof(frequency_bcd));
        break;

    case 0x07: /* VFO and dual watch queries */
        payload[0] = cmd;
        payload[1] = frame[5];
        payload[2] = 0x00;
        reply(fd, payload, 3);
        break;

    case 0x14: /* levels */
        payload[0] = cmd;
        payload[1] = frame[5];
        payload[2] = 0x00;
        payload[3] = 0x00;
        reply(fd, payload, 4);
        break;

    default:
        payload[0] = CIV_OK;
        reply(fd, payload, 1);
        break;
    }
}

static long now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* Reads CI-V frames from the controller and answers them; with stream_scope
 * set, also sends a scope frame every SCOPE_FRAME_INTERVAL_MS, whether or not
 * the controller is reading, and misses the first command, which it never
 * answers. */
static int emulate(int fd, int stream_scope)
{
    unsigned char frame[CIV_FRAME_MAX];
    struct pollfd pfd;
    long next_scope_frame = now_ms();
    int answering = !stream_scope;
    int len = 0;

    pfd.fd = fd;
    pfd.events = POLLIN;

    for (;;)
    {
        unsigned char c;
        int wait_ms = -1;

        if (stream_scope)
        {
            long now = now_ms();

            if (now >= next_scope_frame)
            {
                send_scope_frame(fd);
                next_scope_frame = now + SCOPE_FRAME_INTERVAL_MS;
            }

            wait_ms = (int)(next_scope_frame - now);
        }

        if (poll(&pfd, 1, wait_ms) <= 0 || !(pfd.revents & POLLIN))
        {
            continue;
        }

        if (read(fd, &c, 1) != 1)
        {
            return 0;
        }

        if (len == 0 && c != CIV_PREAMBLE)
        {
            continue;
        }

        if (len < CIV_FRAME_MAX)
        {
            frame[len++] = c;
        }

        if (c == CIV_END)
        {
            if (answering)
            {
                answer(fd, frame, len);
            }

            answering = 1;
            len = 0;
        }
    }
}

static void set_raw(int fd)
{
    struct termios tio;

    if (tcgetattr(fd, &tio) == 0)
    {
        tio.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL
                         | IXON);
        tio.c_oflag &= ~OPOST;
        tio.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
        tcsetattr(fd, TCSANOW, &tio);
    }
}

static pid_t emulator;

static void give_up(int sig)
{
    static const char message[] = "FAIL: rig_open() did not return within "
                                  "the time limit\n";

    (void)sig;

    if (write(STDOUT_FILENO, message, sizeof(message) - 1) < 0) {}

    kill(emulator, SIGKILL);
    _exit(1);
}

/* Opens an emulated IC-7610 with async data enabled and reads its frequency */
static int open_emulated_rig(int stream_scope)
{
    const char *slave_name;
    freq_t freq = 0;
    int master, guard, status;
    RIG *rig;
    int failures = 0;

    master = posix_openpt(O_RDWR | O_NOCTTY);

    if (master < 0 || grantpt(master) < 0 || unlockpt(master) < 0)
    {
        perror("posix_openpt");
        return 1;
    }

    slave_name = ptsname(master);

    if (slave_name == NULL || (guard = open(slave_name, O_RDWR | O_NOCTTY)) < 0)
    {
        perror("open pty slave");
        close(master);
        return 1;
    }

    /* Raw from the start, so the line discipline echoes none of the rig's
     * frames back to it before the backend sets the port up */
    set_raw(guard);
    emulator = fork();

    if (emulator < 0)
    {
        perror("fork");
        close(guard);
        close(master);
        return 1;
    }

    if (emulator == 0)
    {
        close(guard);
        _exit(emulate(master, stream_scope));
    }

    close(master);
    rig = rig_init(RIG_MODEL_IC7610);
    failures += expect(rig != NULL, "initialize IC-7610 backend");

    if (rig == NULL)
    {
        kill(emulator, SIGTERM);
        waitpid(emulator, NULL, 0);
        close(guard);
        return 1;
    }

    rig_set_conf(rig, rig_token_lookup(rig, "rig_pathname"), slave_name);
    rig_set_conf(rig, rig_token_lookup(rig, "async"), "1");
    alarm(OPEN_TIME_LIMIT_S);
    status = rig_open(rig);
    alarm(0);
    failures += expect(status == RIG_OK, stream_scope
                       ? "open IC-7610 emulator streaming scope data with async data enabled"
                       : "open IC-7610 emulator with async data enabled");
    close(guard);

    if (status == RIG_OK)
    {
        failures += expect(rig_get_freq(rig, RIG_VFO_CURR, &freq) == RIG_OK
                           && freq == 14074000,
                           "read the frequency through the async data handler");
        rig_close(rig);
    }

    rig_cleanup(rig);
    kill(emulator, SIGTERM);
    waitpid(emulator, NULL, 0);

    return failures;
}

int main(void)
{
    int failures = 0;

    /* Line buffered, so the results so far are not lost if the time limit ends the test */
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGALRM, give_up);
    rig_set_debug_level(RIG_DEBUG_NONE);
    rig_load_backend("icom");

    failures += open_emulated_rig(0);
    failures += open_emulated_rig(1);

    return failures == 0 ? 0 : 1;
}

#endif
