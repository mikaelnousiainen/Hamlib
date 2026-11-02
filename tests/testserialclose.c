/*
 * Closes an emulated IC-7610's serial port and checks the settings it leaves.
 *
 * Linux keeps a serial port's settings from one open to the next, and a USB
 * serial bridge applies them as the port is opened, before the program that
 * opened it sets its own. When the settings left behind have XON/XOFF on, as a
 * tty's default settings do, a CP210x bridge acts on the rig's 0x13 bytes in
 * that moment: its transmitter can stay held after the program has switched
 * XON/XOFF off, and the program's first command is never sent.
 *
 * The port is set up as another program could have left it, XON/XOFF on in
 * both directions among other settings that differ from Hamlib's own, and the
 * rig is opened and closed. While it is open, without a handshake, XON/XOFF
 * must be off both ways. The close must leave XON/XOFF off and every other
 * setting as it was before the open, whether the rig opened or never answered.
 */

#if defined(_WIN32) || defined(WIN32)

int main(void)
{
    return 77;
}

#else

#define _XOPEN_SOURCE 600
#define _DARWIN_C_SOURCE

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "hamlib/rig.h"

#define CIV_PREAMBLE 0xfe
#define CIV_END 0xfd
#define CIV_RIG_ADDRESS 0x98
#define CIV_CONTROLLER_ADDRESS 0xe0
#define CIV_OK 0xfb
#define CIV_FRAME_MAX 64

/* The emulated rig opens in about two seconds; one that never answers fails
 * to open in about three */
#define OPEN_TIME_LIMIT_S 20

/* What the emulated rig does with the commands it is sent */
enum rig_behavior
{
    RIG_ANSWERS,
    RIG_SILENT,
};

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

/* Answers one CI-V command an IC-7610 backend sends while opening, without
 * echoing it, as a rig with USB echo off does. */
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

/* Reads CI-V frames from the controller and answers them, or reads them and
 * answers nothing */
static int emulate(int fd, enum rig_behavior behavior)
{
    unsigned char frame[CIV_FRAME_MAX];
    int len = 0;

    for (;;)
    {
        unsigned char c;

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
            if (behavior == RIG_ANSWERS)
            {
                answer(fd, frame, len);
            }

            len = 0;
        }
    }
}

/* Sets the port up as another program could have left it: raw, so the line
 * discipline echoes nothing back to the rig, with XON/XOFF on in both
 * directions, and with a speed, output processing, input translation and read
 * timing of its own that Hamlib changes while the port is open. */
static int set_left_by_another_program(int fd, struct termios *left)
{
    struct termios tio;

    if (tcgetattr(fd, &tio) != 0)
    {
        perror("tcgetattr");
        return -1;
    }

    tio.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR);
    tio.c_iflag |= ICRNL | IXON | IXOFF | IXANY;
    tio.c_oflag |= OPOST | ONLCR;
    tio.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    tio.c_cc[VMIN] = 3;
    tio.c_cc[VTIME] = 7;
    cfsetispeed(&tio, B4800);
    cfsetospeed(&tio, B4800);

    if (tcsetattr(fd, TCSANOW, &tio) != 0 || tcgetattr(fd, left) != 0)
    {
        perror("tcsetattr");
        return -1;
    }

    return 0;
}

/* Whether the port's settings are those left by the other program, XON/XOFF
 * aside */
static int same_but_xonxoff(const struct termios *a, const struct termios *b)
{
    int i;

    if ((a->c_iflag & ~(IXON | IXOFF)) != (b->c_iflag & ~(IXON | IXOFF))
            || a->c_oflag != b->c_oflag
            || a->c_cflag != b->c_cflag
            || a->c_lflag != b->c_lflag
            || cfgetispeed(a) != cfgetispeed(b)
            || cfgetospeed(a) != cfgetospeed(b))
    {
        return 0;
    }

    for (i = 0; i < NCCS; i++)
    {
        if (a->c_cc[i] != b->c_cc[i])
        {
            return 0;
        }
    }

    return 1;
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

/* Opens and closes an emulated IC-7610 on a port another program left with
 * XON/XOFF on, and checks the settings the close leaves */
static int open_and_close(enum rig_behavior behavior, const char *description)
{
    struct termios left, after;
    const char *slave_name;
    int master, guard, status;
    RIG *rig;
    int failures = 0;

    printf("%s\n", description);
    master = posix_openpt(O_RDWR | O_NOCTTY);

    if (master < 0 || grantpt(master) < 0 || unlockpt(master) < 0)
    {
        perror("posix_openpt");
        return 1;
    }

    slave_name = ptsname(master);

    /* The guard keeps the pty's settings between Hamlib's open and close and
     * reads them afterwards */
    if (slave_name == NULL || (guard = open(slave_name, O_RDWR | O_NOCTTY)) < 0)
    {
        perror("open pty slave");
        close(master);
        return 1;
    }

    if (set_left_by_another_program(guard, &left) != 0)
    {
        close(guard);
        close(master);
        return 1;
    }

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
        _exit(emulate(master, behavior));
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
    alarm(OPEN_TIME_LIMIT_S);
    status = rig_open(rig);
    alarm(0);

    if (behavior == RIG_ANSWERS)
    {
        failures += expect(status == RIG_OK, "the rig opens");

        if (status == RIG_OK)
        {
            struct termios open_settings;

            failures += expect(tcgetattr(guard, &open_settings) == 0
                               && cfgetospeed(&open_settings) == B115200,
                               "the port runs at the rig's 115200 while the rig is open");
            failures += expect(!(open_settings.c_iflag & (IXON | IXOFF)),
                               "XON/XOFF is off both ways while the rig is open without a handshake");
            failures += expect(rig_close(rig) == RIG_OK, "the rig closes");
        }
    }
    else
    {
        failures += expect(status != RIG_OK, "the rig that never answers fails to open");
    }

    failures += expect(tcgetattr(guard, &after) == 0, "read the port's settings after the close");
    failures += expect(!(after.c_iflag & IXON),
                       "the close leaves XON/XOFF off for the output (IXON)");
    failures += expect(!(after.c_iflag & IXOFF),
                       "the close leaves XON/XOFF off for the input (IXOFF)");
    failures += expect(same_but_xonxoff(&after, &left),
                       "the close restores every other setting the port had before the open");

    rig_cleanup(rig);
    kill(emulator, SIGTERM);
    waitpid(emulator, NULL, 0);
    close(guard);

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

    failures += open_and_close(RIG_ANSWERS, "An IC-7610 opened and closed:");
    failures += open_and_close(RIG_SILENT, "An IC-7610 that never answers:");

    return failures == 0 ? 0 : 1;
}

#endif
