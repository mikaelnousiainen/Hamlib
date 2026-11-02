/*
 * Opens an emulated IC-7610 with async data enabled.
 *
 * With async data enabled, replies are read from the sync data pipe that the
 * async data handler fills from the port. That handler starts only after the
 * backend's rig_open() has finished, so the transactions the backend makes
 * while opening, such as the Icom USB echo check, must read the port itself;
 * otherwise they time out and the rig cannot be opened at all.
 */

#if defined(_WIN32) || defined(WIN32)

int main(void)
{
    return 77;
}

#else

#define _XOPEN_SOURCE 600

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "hamlib/rig.h"

#define CIV_PREAMBLE 0xfe
#define CIV_END 0xfd
#define CIV_RIG_ADDRESS 0x98
#define CIV_CONTROLLER_ADDRESS 0xe0
#define CIV_OK 0xfb
#define CIV_FRAME_MAX 64

/* 14.074000 MHz as five BCD bytes, least significant first */
static const unsigned char frequency_bcd[5] = { 0x00, 0x40, 0x07, 0x14, 0x00 };

static int expect(int condition, const char *description)
{
    printf("%s: %s\n", condition ? "PASS" : "FAIL", description);
    return condition ? 0 : 1;
}

static int read_frame(int fd, unsigned char *frame, int max)
{
    int len = 0;
    unsigned char c;

    while (read(fd, &c, 1) == 1)
    {
        if (len == 0 && c != CIV_PREAMBLE)
        {
            continue;
        }

        if (len < max)
        {
            frame[len++] = c;
        }

        if (c == CIV_END)
        {
            return len;
        }
    }

    return -1;
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

/* Answers the CI-V commands an IC-7610 backend sends while opening and reading
 * the frequency, without echoing them, as a rig with USB echo off does. */
static int emulate(int fd)
{
    unsigned char frame[CIV_FRAME_MAX];
    unsigned char payload[CIV_FRAME_MAX];
    int len;

    while ((len = read_frame(fd, frame, sizeof(frame))) > 0)
    {
        unsigned char cmd;

        if (len < 6 || frame[2] != CIV_RIG_ADDRESS)
        {
            continue;
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

    return 0;
}

int main(void)
{
    const char *slave_name;
    freq_t freq = 0;
    int master, guard, status;
    pid_t child;
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

    child = fork();

    if (child < 0)
    {
        perror("fork");
        close(guard);
        close(master);
        return 1;
    }

    if (child == 0)
    {
        close(guard);
        _exit(emulate(master));
    }

    close(master);
    rig_set_debug_level(RIG_DEBUG_NONE);
    rig_load_backend("icom");
    rig = rig_init(RIG_MODEL_IC7610);
    failures += expect(rig != NULL, "initialize IC-7610 backend");

    if (rig == NULL)
    {
        kill(child, SIGTERM);
        waitpid(child, NULL, 0);
        close(guard);
        return 1;
    }

    rig_set_conf(rig, rig_token_lookup(rig, "rig_pathname"), slave_name);
    rig_set_conf(rig, rig_token_lookup(rig, "async"), "1");
    status = rig_open(rig);
    failures += expect(status == RIG_OK, "open IC-7610 emulator with async data enabled");
    close(guard);

    if (status == RIG_OK)
    {
        failures += expect(rig_get_freq(rig, RIG_VFO_CURR, &freq) == RIG_OK
                           && freq == 14074000,
                           "read the frequency through the async data handler");
        rig_close(rig);
    }

    rig_cleanup(rig);
    kill(child, SIGTERM);
    waitpid(child, NULL, 0);

    return failures == 0 ? 0 : 1;
}

#endif
