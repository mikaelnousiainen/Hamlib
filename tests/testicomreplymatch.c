/*
 * Reads an emulated IC-7610 after it has answered a command late.
 *
 * The emulated rig answers its first read of the AF gain (14 01) only after
 * the backend's read of the reply has timed out, or sends the first bytes of
 * that reply at once and the rest only after the timeout. The backend sends
 * the command again and takes the reply to the second one, so a reply is left
 * over, and it arrives while a later command waits for its own. A transaction
 * that took it would return the AF gain as that command's value, and every
 * transaction after it the reply to the command before it.
 *
 * Every read the emulator answers gives a value that differs from command to
 * command, so a reply taken by the wrong command shows as a wrong value. After
 * the late reply the test reads the frequency, mode, three levels, a function
 * and PTT, and each must be the rig's own.
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

/* The IC-7610 backend's reply timeout is 1000 ms */
#define LATE_REPLY_MS 1500
/* Bytes of the late reply sent at once when it is split */
#define SPLIT_REPLY_HEAD_LEN 5

#define TIME_LIMIT_S 30

/* How the emulated rig answers its first read of the AF gain */
enum late_reply
{
    LATE_REPLY_WHOLE,
    LATE_REPLY_SPLIT,
};

/* 14.074000 MHz as five BCD bytes, least significant first */
static const unsigned char frequency_bcd[5] = { 0x00, 0x40, 0x07, 0x14, 0x00 };

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

static void send_all(int fd, const unsigned char *bytes, int len)
{
    if (write(fd, bytes, len) != len)
    {
        _exit(2);
    }
}

/* Builds the frame the rig sends with this payload and returns its length */
static int build_frame(unsigned char *frame, const unsigned char *payload,
                       int payload_len)
{
    int len = 0;

    frame[len++] = CIV_PREAMBLE;
    frame[len++] = CIV_PREAMBLE;
    frame[len++] = CIV_CONTROLLER_ADDRESS;
    frame[len++] = CIV_RIG_ADDRESS;
    memcpy(frame + len, payload, payload_len);
    len += payload_len;
    frame[len++] = CIV_END;

    return len;
}

/* Builds the reply to one command and returns its length. A read of a level
 * (14 xx) answers the subcommand times 20, so each level differs; every
 * function (16 xx) is on and PTT (1c 00) off. */
static int build_reply(const unsigned char *command, int len,
                       unsigned char *frame)
{
    unsigned char payload[CIV_FRAME_MAX];
    unsigned char cmd = command[4];
    unsigned char subcmd = command[5];
    int read_with_subcmd = len == 7;
    int n = 0;

    payload[n++] = cmd;

    switch (cmd)
    {
    case 0x03: /* read the operating frequency */
        memcpy(payload + n, frequency_bcd, sizeof(frequency_bcd));
        n += sizeof(frequency_bcd);
        break;

    case 0x25: /* read the selected or unselected VFO's frequency */
        if (!read_with_subcmd) { payload[0] = CIV_OK; break; }

        payload[n++] = subcmd;
        memcpy(payload + n, frequency_bcd, sizeof(frequency_bcd));
        n += sizeof(frequency_bcd);
        break;

    case 0x26: /* read the selected or unselected VFO's mode: USB, data off, FIL1 */
        if (!read_with_subcmd) { payload[0] = CIV_OK; break; }

        payload[n++] = subcmd;
        payload[n++] = 0x01;
        payload[n++] = 0x00;
        payload[n++] = 0x01;
        break;

    case 0x07: /* VFO and dual watch queries */
        payload[n++] = subcmd;
        payload[n++] = 0x00;
        break;

    case 0x14: /* read a level */
        if (!read_with_subcmd) { payload[0] = CIV_OK; break; }

        payload[n++] = subcmd;
        payload[n++] = (unsigned char)(subcmd * 20 / 100);
        payload[n++] = (unsigned char)((subcmd * 20 / 10 % 10) << 4 | subcmd * 20 % 10);
        break;

    case 0x16: /* read a function */
        if (!read_with_subcmd) { payload[0] = CIV_OK; break; }

        payload[n++] = subcmd;
        payload[n++] = 0x01;
        break;

    case 0x1a: /* read the filter width, index 31 */
        if (!read_with_subcmd || subcmd != 0x03) { payload[0] = CIV_OK; break; }

        payload[n++] = subcmd;
        payload[n++] = 0x31;
        break;

    case 0x1c: /* read PTT */
        if (!read_with_subcmd) { payload[0] = CIV_OK; break; }

        payload[n++] = subcmd;
        payload[n++] = 0x00;
        break;

    default:
        payload[0] = CIV_OK;
        break;
    }

    return build_frame(frame, payload, n);
}

/* Reads CI-V frames from the controller and answers them at once, except the
 * first read of the AF gain, which it answers late as late_reply says */
static int emulate(int fd, enum late_reply late_reply)
{
    unsigned char command[CIV_FRAME_MAX];
    unsigned char pending[CIV_FRAME_MAX];
    struct pollfd pfd;
    long pending_due = 0;
    int pending_len = 0;
    int answered_late = 0;
    int len = 0;

    pfd.fd = fd;
    pfd.events = POLLIN;

    for (;;)
    {
        unsigned char c;
        int wait_ms = -1;

        if (pending_len > 0)
        {
            long now = now_ms();

            if (now >= pending_due)
            {
                send_all(fd, pending, pending_len);
                pending_len = 0;
                continue;
            }

            wait_ms = (int)(pending_due - now);
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
            command[len++] = c;
        }

        if (c != CIV_END)
        {
            continue;
        }

        if (len >= 6 && command[2] == CIV_RIG_ADDRESS)
        {
            unsigned char reply[CIV_FRAME_MAX];
            int reply_len = build_reply(command, len, reply);
            int head_len = 0;

            if (!answered_late && len == 7 && command[4] == 0x14 && command[5] == 0x01)
            {
                answered_late = 1;
                head_len = late_reply == LATE_REPLY_SPLIT ? SPLIT_REPLY_HEAD_LEN : 0;
                send_all(fd, reply, head_len);
                pending_len = reply_len - head_len;
                memcpy(pending, reply + head_len, pending_len);
                pending_due = now_ms() + LATE_REPLY_MS;
            }
            else
            {
                send_all(fd, reply, reply_len);
            }
        }

        len = 0;
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
    static const char message[] = "FAIL: the test did not finish within the "
                                  "time limit\n";

    (void)sig;

    if (write(STDOUT_FILENO, message, sizeof(message) - 1) < 0) {}

    kill(emulator, SIGKILL);
    _exit(1);
}

/* Reads a level as the raw value the rig sent */
static int read_level(RIG *rig, setting_t level, int *raw)
{
    value_t val;
    int status = rig_get_level(rig, RIG_VFO_CURR, level, &val);

    *raw = (int)(val.f * 255.0f + 0.5f);
    return status;
}

/* Reads everything once more after the late reply has arrived; each value
 * must be the rig's own */
static int read_after_late_reply(RIG *rig)
{
    freq_t freq = 0;
    rmode_t mode = RIG_MODE_NONE;
    pbwidth_t width = 0;
    ptt_t ptt = RIG_PTT_ON;
    int failures = 0;
    int raw = 0;
    int on = 0;
    int status;

    status = rig_get_freq(rig, RIG_VFO_CURR, &freq);
    failures += expect(status == RIG_OK && freq == 14074000,
                       "frequency is the rig's, 14074000");

    status = rig_get_mode(rig, RIG_VFO_CURR, &mode, &width);
    failures += expect(status == RIG_OK && mode == RIG_MODE_USB,
                       "mode is the rig's, USB");

    status = read_level(rig, RIG_LEVEL_AF, &raw);
    failures += expect(status == RIG_OK && raw == 20, "AF gain is the rig's, 20");

    status = read_level(rig, RIG_LEVEL_RF, &raw);
    failures += expect(status == RIG_OK && raw == 40, "RF gain is the rig's, 40");

    status = read_level(rig, RIG_LEVEL_RFPOWER, &raw);
    failures += expect(status == RIG_OK && raw == 200, "RF power is the rig's, 200");

    status = rig_get_func(rig, RIG_VFO_CURR, RIG_FUNC_NB, &on);
    failures += expect(status == RIG_OK && on == 1, "noise blanker is the rig's, on");

    status = rig_get_ptt(rig, RIG_VFO_CURR, &ptt);
    failures += expect(status == RIG_OK && ptt == RIG_PTT_OFF,
                       "PTT is the rig's, off");

    return failures;
}

static int read_after_late_answer(enum late_reply late_reply, const char *async,
                                  const char *description)
{
    const char *slave_name;
    int master, guard, status;
    RIG *rig;
    int raw = 0;
    int failures = 0;

    printf("%s\n", description);

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
        _exit(emulate(master, late_reply));
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
    rig_set_conf(rig, rig_token_lookup(rig, "async"), async);
    status = rig_open(rig);
    failures += expect(status == RIG_OK, "open IC-7610 emulator");
    close(guard);

    if (status == RIG_OK)
    {
        /* Every read goes to the rig */
        rig_set_cache_timeout_ms(rig, HAMLIB_CACHE_ALL, 0);

        status = read_level(rig, RIG_LEVEL_AF, &raw);
        failures += expect(status == RIG_OK && raw == 20,
                           "AF gain read again after the rig answered late");

        /* Until the late reply has arrived */
        usleep(LATE_REPLY_MS * 1000);

        failures += read_after_late_reply(rig);
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
    alarm(TIME_LIMIT_S);

    failures += read_after_late_answer(LATE_REPLY_WHOLE, "0",
                                       "reply after the timeout, reading the port");
    failures += read_after_late_answer(LATE_REPLY_WHOLE, "1",
                                       "reply after the timeout, with async data enabled");
    failures += read_after_late_answer(LATE_REPLY_SPLIT, "0",
                                       "reply cut short by the timeout, reading the port");

    return failures == 0 ? 0 : 1;
}

#endif
