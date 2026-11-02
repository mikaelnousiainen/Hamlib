/*
 * Opens an emulated IC-7610 that is already open, with async data enabled.
 *
 * rig_open() on a rig that is already open closes it and fails. The rig's
 * async data handler, its keyer and poll threads read and use its port while
 * it is open; closed, it must be closed whole, those threads stopped, or they
 * run on with the port closed under them, and the next open starts a second
 * set beside them: two threads reading one port, each taking part of what the
 * rig sends.
 *
 * The test counts the process's threads: closed by the second rig_open(), the
 * rig must leave as many as before it was first opened, and opened a third
 * time, as many as it had open the first time, its frequency read as before.
 */

#if defined(_WIN32) || defined(WIN32)

int main(void)
{
    return 77;
}

#else

#define _XOPEN_SOURCE 600

#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
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

#define TIME_LIMIT_S 30

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

/* Reads CI-V frames from the controller and answers them */
static int emulate(int fd)
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
            answer(fd, frame, len);
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

/* The number of threads this process runs */
static int thread_count(void)
{
    DIR *dir = opendir("/proc/self/task");
    struct dirent *entry;
    int count = 0;

    if (dir == NULL)
    {
        return -1;
    }

    while ((entry = readdir(dir)) != NULL)
    {
        if (entry->d_name[0] != '.')
        {
            count++;
        }
    }

    closedir(dir);
    return count;
}

/* The number of threads after those a stopping thread leaves have gone */
static int settled_thread_count(void)
{
    struct timespec pause = { 0, 100 * 1000 * 1000 };

    nanosleep(&pause, NULL);
    return thread_count();
}

static pid_t emulator;

static void give_up(int sig)
{
    static const char message[] = "FAIL: the test did not finish within the time limit\n";

    (void)sig;

    if (write(STDOUT_FILENO, message, sizeof(message) - 1) < 0) {}

    kill(emulator, SIGKILL);
    _exit(1);
}

int main(void)
{
    const char *slave_name;
    freq_t freq = 0;
    int master, guard;
    int closed_threads, open_threads, status;
    RIG *rig;
    int failures = 0;

    /* Line buffered, so the results so far are not lost if the time limit ends the test */
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGALRM, give_up);
    rig_set_debug_level(RIG_DEBUG_NONE);
    rig_load_backend("icom");

    if (thread_count() < 1)
    {
        printf("SKIP: /proc/self/task cannot be read\n");
        return 77;
    }

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
        return 1;
    }

    if (emulator == 0)
    {
        close(guard);
        _exit(emulate(master));
    }

    close(master);
    rig = rig_init(RIG_MODEL_IC7610);

    if (rig == NULL)
    {
        expect(0, "initialize IC-7610 backend");
        kill(emulator, SIGTERM);
        waitpid(emulator, NULL, 0);
        return 1;
    }

    rig_set_conf(rig, rig_token_lookup(rig, "rig_pathname"), slave_name);
    rig_set_conf(rig, rig_token_lookup(rig, "async"), "1");
    alarm(TIME_LIMIT_S);
    closed_threads = thread_count();

    status = rig_open(rig);
    failures += expect(status == RIG_OK, "open IC-7610 emulator with async data enabled");
    open_threads = settled_thread_count();
    printf("threads: %d before the open, %d with the rig open\n", closed_threads, open_threads);

    status = rig_open(rig);
    failures += expect(status != RIG_OK, "opening the rig again fails");
    status = settled_thread_count();
    printf("threads after the second open: %d\n", status);
    failures += expect(status == closed_threads,
                       "the second open closes the rig whole: none of its threads is left running");

    /* Hamlib opens the port for its own use alone (TIOCEXCL), which a serial
     * port's last close ends; the guard keeps the pty open, so it ends it */
    ioctl(guard, TIOCNXCL, NULL);
    status = rig_open(rig);
    failures += expect(status == RIG_OK, "open the rig a third time");
    status = settled_thread_count();
    printf("threads after the third open: %d\n", status);
    failures += expect(status == open_threads,
                       "the third open starts one set of threads, as the first did");
    failures += expect(rig_get_freq(rig, RIG_VFO_CURR, &freq) == RIG_OK
                       && freq == 14074000,
                       "read the frequency through the async data handler");

    rig_close(rig);
    status = settled_thread_count();
    failures += expect(status == closed_threads, "closing the rig stops all its threads");
    alarm(0);

    rig_cleanup(rig);
    kill(emulator, SIGTERM);
    waitpid(emulator, NULL, 0);
    close(guard);

    return failures == 0 ? 0 : 1;
}

#endif
