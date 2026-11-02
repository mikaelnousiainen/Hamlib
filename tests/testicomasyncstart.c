/*
 * Opens an emulated IC-7610 that streams scope data, with async data enabled,
 * and checks that none of what it sends is lost while the rig opens.
 *
 * A rig without flow control does not wait for its controller: a serial port
 * holds 4 KB of what it receives, and what comes while the buffer is full is
 * lost. An IC-7610 with its scope data output on sends about 62 scope frames
 * a second, unasked, so a controller that stops reading for a little over a
 * second loses data, and the frames on either side of the gap run together,
 * with any reply among them. With async data enabled, the async data handler
 * reads the port once the rig is open; nothing may leave the port unread
 * before it starts.
 *
 * The emulator keeps such a buffer: it sends only what the controller's
 * unread 4 KB has room for, as a serial port would keep it, and counts the
 * bytes it drops. Every byte must be read: none dropped during the open or
 * in the second after it, and the frequency read afterwards.
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
#include <sys/ioctl.h>
#include <sys/mman.h>
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

/* An IC-7610 with its scope data output on sends about 62 scope frames a
 * second, most of them 60 bytes long */
#define SCOPE_FRAME_INTERVAL_MS 16
#define SCOPE_FRAME_DATA_LEN 50
#define SCOPE_FRAME_SEQUENCE_MAX 0x15

/* What a serial port holds of what it has received and not yet been read */
#define PORT_BUFFER_LEN 4096

/* How long the rig is kept open after rig_open() returns */
#define OPEN_FOR_MS 1000

#define OPEN_TIME_LIMIT_S 20

/* 14.074000 MHz as five BCD bytes, least significant first */
static const unsigned char frequency_bcd[5] = { 0x00, 0x40, 0x07, 0x14, 0x00 };

/* The bytes the emulator could not send, shared with the test */
static volatile long *dropped;

/* The controller's side of the pty, which the emulator asks how much of what
 * it sent is still unread */
static int controller_side = -1;

static int expect(int condition, const char *description)
{
    printf("%s: %s\n", condition ? "PASS" : "FAIL", description);
    return condition ? 0 : 1;
}

static long now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* Sends as much of a frame as the controller's unread buffer has room for,
 * as a serial port receiving it would keep it, and counts the rest as lost */
static void send_frame(int fd, const unsigned char *frame, int len)
{
    int unread = 0;
    int room;

    if (ioctl(controller_side, FIONREAD, &unread) < 0)
    {
        unread = 0;
    }

    room = PORT_BUFFER_LEN - unread;

    if (room < 0)
    {
        room = 0;
    }

    if (room < len)
    {
        *dropped += len - room;
        len = room;
    }

    if (len > 0 && write(fd, frame, len) != len)
    {
        _exit(2);
    }
}

static void reply(int fd, const unsigned char *payload, int payload_len)
{
    unsigned char frame[CIV_FRAME_MAX + SCOPE_FRAME_DATA_LEN];
    int len = 0;

    frame[len++] = CIV_PREAMBLE;
    frame[len++] = CIV_PREAMBLE;
    frame[len++] = CIV_CONTROLLER_ADDRESS;
    frame[len++] = CIV_RIG_ADDRESS;
    memcpy(frame + len, payload, payload_len);
    len += payload_len;
    frame[len++] = CIV_END;

    send_frame(fd, frame, len);
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

/* Answers the controller's CI-V commands and sends a scope frame every
 * SCOPE_FRAME_INTERVAL_MS, whether or not the controller is reading */
static int emulate(int fd)
{
    unsigned char frame[CIV_FRAME_MAX];
    struct pollfd pfd;
    long next_scope_frame = now_ms();
    int len = 0;

    pfd.fd = fd;
    pfd.events = POLLIN;

    for (;;)
    {
        unsigned char c;
        long now = now_ms();

        if (now >= next_scope_frame)
        {
            send_scope_frame(fd);
            next_scope_frame = now + SCOPE_FRAME_INTERVAL_MS;
        }

        if (poll(&pfd, 1, (int)(next_scope_frame - now)) <= 0
                || !(pfd.revents & POLLIN))
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

int main(void)
{
    const char *slave_name;
    freq_t freq = 0;
    int master, status;
    RIG *rig;
    int failures = 0;

    /* Line buffered, so the results so far are not lost if the time limit ends the test */
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGALRM, give_up);
    rig_set_debug_level(RIG_DEBUG_NONE);
    rig_load_backend("icom");

    dropped = mmap(NULL, sizeof(*dropped), PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_ANONYMOUS, -1, 0);

    if (dropped == MAP_FAILED)
    {
        perror("mmap");
        return 1;
    }

    *dropped = 0;
    master = posix_openpt(O_RDWR | O_NOCTTY);

    if (master < 0 || grantpt(master) < 0 || unlockpt(master) < 0)
    {
        perror("posix_openpt");
        return 1;
    }

    slave_name = ptsname(master);

    if (slave_name == NULL
            || (controller_side = open(slave_name, O_RDWR | O_NOCTTY)) < 0)
    {
        perror("open pty slave");
        close(master);
        return 1;
    }

    /* Raw from the start, so the line discipline echoes none of the rig's
     * frames back to it before the backend sets the port up */
    set_raw(controller_side);
    emulator = fork();

    if (emulator < 0)
    {
        perror("fork");
        return 1;
    }

    if (emulator == 0)
    {
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
    alarm(OPEN_TIME_LIMIT_S);
    status = rig_open(rig);
    alarm(0);
    failures += expect(status == RIG_OK,
                       "open IC-7610 emulator streaming scope data with async data enabled");

    if (status == RIG_OK)
    {
        long open_at = now_ms();

        while (now_ms() - open_at < OPEN_FOR_MS)
        {
            usleep(10 * 1000);
        }

        printf("bytes the rig could not send: %ld\n", *dropped);
        failures += expect(*dropped == 0,
                           "nothing the rig sent was lost while it was opened and in the second after");
        failures += expect(rig_get_freq(rig, RIG_VFO_CURR, &freq) == RIG_OK
                           && freq == 14074000,
                           "read the frequency through the async data handler");
        rig_close(rig);
    }

    rig_cleanup(rig);
    kill(emulator, SIGTERM);
    waitpid(emulator, NULL, 0);
    close(controller_side);

    return failures == 0 ? 0 : 1;
}

#endif
