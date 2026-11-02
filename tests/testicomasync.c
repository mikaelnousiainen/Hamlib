/*
 * Drives an emulated IC-7610 with async data enabled.
 *
 * With async data enabled, replies are read from the sync data pipe that the
 * async data handler fills from the port. That handler starts only after the
 * backend's rig_open() has finished, so the transactions the backend makes
 * while opening, such as the Icom USB echo check, must read the port itself;
 * otherwise they time out and the rig cannot be opened at all.
 *
 * Once the rig is open, the emulator pushes CI-V transceive frequency frames
 * and two spectrum scope lines, as an IC-7610 does over its USB serial port.
 * The async data handler must hand them to the frequency and spectrum
 * callbacks, and a later solicited reply must still reach the caller through
 * the sync data pipe.
 */

#if defined(_WIN32) || defined(WIN32)

int main(void)
{
    return 77;
}

#else

#define _XOPEN_SOURCE 600

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "hamlib/rig.h"

#define CIV_PREAMBLE 0xfe
#define CIV_END 0xfd
#define CIV_BROADCAST_ADDRESS 0x00
#define CIV_RIG_ADDRESS 0x98
#define CIV_CONTROLLER_ADDRESS 0xe0
#define CIV_OK 0xfb
#define CIV_FRAME_MAX 64

#define CIV_SCOPE_MODE_CENTER 0x00
#define CIV_SCOPE_MODE_FIXED 0x01

/* IC-7610 spectrum lines over USB: a header frame followed by 14 data frames
 * of up to 50 bytes each, 689 bytes in all */
#define SCOPE_LINE_LENGTH 689
#define SCOPE_CHUNK_LENGTH 50
#define SCOPE_DIVISIONS 15
#define SCOPE_DATA_LEVEL_MAX 200

#define PUSH_COMMAND 'P'
#define EVENT_TIMEOUT_SECONDS 3

/* rig_fire_freq_event() drops frequency events that follow the previous one
 * within 250 ms, and the first one ever, so only the second push is passed
 * on */
#define FREQ_PUSH_INTERVAL_NS 300000000L

#define SOLICITED_FREQ 14074000
#define PUSHED_FREQ_FIRST 7074000
#define PUSHED_FREQ_SECOND 7075500

#define CENTER_LINE_CENTER 7100000
#define CENTER_LINE_HALF_SPAN 25000
#define CENTER_LINE_LOW_EDGE (CENTER_LINE_CENTER - CENTER_LINE_HALF_SPAN)
#define CENTER_LINE_HIGH_EDGE (CENTER_LINE_CENTER + CENTER_LINE_HALF_SPAN)
#define FIXED_LINE_LOW_EDGE 7000000
#define FIXED_LINE_HIGH_EDGE 7200000

struct emulator
{
    int fd;
    int control_fd;
};

struct spectrum_record
{
    int id;
    enum rig_spectrum_mode_e mode;
    freq_t center_freq;
    freq_t span_freq;
    freq_t low_edge_freq;
    freq_t high_edge_freq;
    int data_level_min;
    int data_level_max;
    size_t data_length;
    unsigned char data[SCOPE_LINE_LENGTH];
};

struct events
{
    pthread_mutex_t lock;
    pthread_cond_t changed;
    int freq_calls;
    vfo_t freq_vfo;
    freq_t freq;
    int spectrum_calls;
    struct spectrum_record spectrum[2];
};

static struct events events =
{
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .changed = PTHREAD_COND_INITIALIZER,
};

static int expect(int condition, const char *description)
{
    printf("%s: %s\n", condition ? "PASS" : "FAIL", description);
    return condition ? 0 : 1;
}

static unsigned char to_bcd_byte(int value)
{
    return (unsigned char)(((value / 10) << 4) | (value % 10));
}

/* Frequencies are five BCD bytes, least significant first */
static void to_bcd_freq(unsigned long hz, unsigned char *bcd)
{
    int i;

    for (i = 0; i < 5; i++)
    {
        bcd[i] = to_bcd_byte((int)(hz % 100));
        hz /= 100;
    }
}

static unsigned char center_line_data(int i)
{
    return (unsigned char)(i % (SCOPE_DATA_LEVEL_MAX + 1));
}

static unsigned char fixed_line_data(int i)
{
    return (unsigned char)((i * 7) % (SCOPE_DATA_LEVEL_MAX + 1));
}

static void send_frame(int fd, unsigned char to, const unsigned char *payload,
                       int payload_len)
{
    unsigned char frame[CIV_FRAME_MAX];
    int len = 0;

    frame[len++] = CIV_PREAMBLE;
    frame[len++] = CIV_PREAMBLE;
    frame[len++] = to;
    frame[len++] = CIV_RIG_ADDRESS;
    memcpy(frame + len, payload, payload_len);
    len += payload_len;
    frame[len++] = CIV_END;

    if (write(fd, frame, len) != len)
    {
        perror("emulator write");
        exit(2);
    }
}

static void reply(int fd, const unsigned char *payload, int payload_len)
{
    send_frame(fd, CIV_CONTROLLER_ADDRESS, payload, payload_len);
}

/* Answers the CI-V commands an IC-7610 backend sends while opening and reading
 * the frequency, without echoing them, as a rig with USB echo off does. */
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
        to_bcd_freq(SOLICITED_FREQ, payload + 1);
        reply(fd, payload, 6);
        break;

    case 0x25: /* read the selected or unselected VFO's frequency */
        payload[0] = cmd;
        payload[1] = frame[5];
        to_bcd_freq(SOLICITED_FREQ, payload + 2);
        reply(fd, payload, 7);
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

static void push_freq(int fd, unsigned long hz)
{
    unsigned char payload[6];

    payload[0] = 0x00; /* transceive: operating frequency changed */
    to_bcd_freq(hz, payload + 1);
    send_frame(fd, CIV_BROADCAST_ADDRESS, payload, sizeof(payload));
}

/* Sends one spectrum line of scope 0 as a header frame and 14 data frames.
 * In center mode the edges are the center frequency and half the span. */
static void push_spectrum_line(int fd, unsigned char mode, unsigned long edge1,
                               unsigned long edge2,
                               unsigned char (*data)(int))
{
    unsigned char payload[CIV_FRAME_MAX];
    int division;
    int offset = 0;

    payload[0] = 0x27; /* scope */
    payload[1] = 0x00; /* waveform data */
    payload[2] = 0x00; /* main scope */
    payload[3] = to_bcd_byte(1);
    payload[4] = to_bcd_byte(SCOPE_DIVISIONS);
    payload[5] = mode;
    to_bcd_freq(edge1, payload + 6);
    to_bcd_freq(edge2, payload + 11);
    payload[16] = 0x00; /* in range */
    send_frame(fd, CIV_CONTROLLER_ADDRESS, payload, 17);

    for (division = 2; division <= SCOPE_DIVISIONS; division++)
    {
        int chunk = SCOPE_LINE_LENGTH - offset;
        int i;

        if (chunk > SCOPE_CHUNK_LENGTH)
        {
            chunk = SCOPE_CHUNK_LENGTH;
        }

        payload[3] = to_bcd_byte(division);

        for (i = 0; i < chunk; i++)
        {
            payload[5 + i] = data(offset + i);
        }

        send_frame(fd, CIV_CONTROLLER_ADDRESS, payload, 5 + chunk);
        offset += chunk;
    }
}

static void push_async_data(int fd)
{
    struct timespec interval = { 0, FREQ_PUSH_INTERVAL_NS };

    push_freq(fd, PUSHED_FREQ_FIRST);
    nanosleep(&interval, NULL);
    push_freq(fd, PUSHED_FREQ_SECOND);
    push_spectrum_line(fd, CIV_SCOPE_MODE_CENTER, CENTER_LINE_CENTER,
                       CENTER_LINE_HALF_SPAN, center_line_data);
    push_spectrum_line(fd, CIV_SCOPE_MODE_FIXED, FIXED_LINE_LOW_EDGE,
                       FIXED_LINE_HIGH_EDGE, fixed_line_data);
}

/* Answers the rig's commands until the control pipe is closed, and pushes the
 * async data when the control pipe says so. */
static void *emulate(void *arg)
{
    const struct emulator *emulator = arg;
    unsigned char frame[CIV_FRAME_MAX];
    int len = 0;

    for (;;)
    {
        struct pollfd fds[2] =
        {
            { .fd = emulator->fd, .events = POLLIN },
            { .fd = emulator->control_fd, .events = POLLIN },
        };
        unsigned char c;

        if (poll(fds, 2, -1) < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }

            perror("emulator poll");
            exit(2);
        }

        if (fds[1].revents)
        {
            if (read(emulator->control_fd, &c, 1) != 1)
            {
                return NULL;
            }

            if (c == PUSH_COMMAND)
            {
                push_async_data(emulator->fd);
            }
        }

        if (!(fds[0].revents & POLLIN) || read(emulator->fd, &c, 1) != 1)
        {
            continue;
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
            answer(emulator->fd, frame, len);
            len = 0;
        }
    }
}

static int freq_event(RIG *rig, vfo_t vfo, freq_t freq, rig_ptr_t arg)
{
    pthread_mutex_lock(&events.lock);
    events.freq_calls++;
    events.freq_vfo = vfo;
    events.freq = freq;
    pthread_cond_broadcast(&events.changed);
    pthread_mutex_unlock(&events.lock);

    return RIG_OK;
}

static int spectrum_event(RIG *rig, struct rig_spectrum_line *line,
                          rig_ptr_t arg)
{
    pthread_mutex_lock(&events.lock);

    if (events.spectrum_calls < 2)
    {
        struct spectrum_record *record;

        record = &events.spectrum[events.spectrum_calls];

        record->id = line->id;
        record->mode = line->spectrum_mode;
        record->center_freq = line->center_freq;
        record->span_freq = line->span_freq;
        record->low_edge_freq = line->low_edge_freq;
        record->high_edge_freq = line->high_edge_freq;
        record->data_level_min = line->data_level_min;
        record->data_level_max = line->data_level_max;
        record->data_length = line->spectrum_data_length;
        memcpy(record->data, line->spectrum_data,
               line->spectrum_data_length < SCOPE_LINE_LENGTH
               ? line->spectrum_data_length : SCOPE_LINE_LENGTH);
    }

    events.spectrum_calls++;
    pthread_cond_broadcast(&events.changed);
    pthread_mutex_unlock(&events.lock);

    return RIG_OK;
}

/* Waits until both spectrum lines have been handled. They are pushed after the
 * frequency frames, so the frequency events have been handled by then too. */
static void wait_for_events(void)
{
    struct timespec deadline;

    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += EVENT_TIMEOUT_SECONDS;

    pthread_mutex_lock(&events.lock);

    while (events.spectrum_calls < 2)
    {
        if (pthread_cond_timedwait(&events.changed, &events.lock,
                                   &deadline) != 0)
        {
            break;
        }
    }

    pthread_mutex_unlock(&events.lock);
}

static int data_matches(const struct spectrum_record *record,
                        unsigned char (*data)(int))
{
    int i;

    if (record->data_length != SCOPE_LINE_LENGTH)
    {
        return 0;
    }

    for (i = 0; i < SCOPE_LINE_LENGTH; i++)
    {
        if (record->data[i] != data(i))
        {
            return 0;
        }
    }

    return 1;
}

static int check_spectrum_line(const struct spectrum_record *record,
                               enum rig_spectrum_mode_e mode, freq_t low_edge,
                               freq_t high_edge, unsigned char (*data)(int),
                               const char *name)
{
    char description[128];
    int failures = 0;

    snprintf(description, sizeof(description), "%s line: scope and mode",
             name);
    failures += expect(record->id == 0 && record->mode == mode, description);
    snprintf(description, sizeof(description), "%s line: frequencies", name);
    failures += expect(record->low_edge_freq == low_edge
                       && record->high_edge_freq == high_edge
                       && record->center_freq == (low_edge + high_edge) / 2
                       && record->span_freq == high_edge - low_edge,
                       description);
    snprintf(description, sizeof(description), "%s line: data levels", name);
    failures += expect(record->data_level_min == 0
                       && record->data_level_max == SCOPE_DATA_LEVEL_MAX,
                       description);
    snprintf(description, sizeof(description),
             "%s line: all divisions' data in order", name);
    failures += expect(data_matches(record, data), description);

    return failures;
}

static int check_events(void)
{
    int failures = 0;

    pthread_mutex_lock(&events.lock);

    failures += expect(events.freq_calls == 1,
                       "one frequency event after the throttle");
    failures += expect(events.freq_calls >= 1
                       && events.freq == PUSHED_FREQ_SECOND
                       && events.freq_vfo == RIG_VFO_CURR,
                       "frequency event carries the latest frequency");
    failures += expect(events.spectrum_calls == 2,
                       "one spectrum event per spectrum line");

    if (events.spectrum_calls >= 2)
    {
        failures += check_spectrum_line(&events.spectrum[0],
                                        RIG_SPECTRUM_MODE_CENTER,
                                        CENTER_LINE_LOW_EDGE,
                                        CENTER_LINE_HIGH_EDGE,
                                        center_line_data, "center mode");
        failures += check_spectrum_line(&events.spectrum[1],
                                        RIG_SPECTRUM_MODE_FIXED,
                                        FIXED_LINE_LOW_EDGE,
                                        FIXED_LINE_HIGH_EDGE,
                                        fixed_line_data, "fixed mode");
    }

    pthread_mutex_unlock(&events.lock);

    return failures;
}

/* Closing the control pipe makes the emulator thread exit */
static void stop_emulator(pthread_t thread, const int control[2], int guard,
                          int master)
{
    close(control[1]);
    pthread_join(thread, NULL);
    close(control[0]);
    close(guard);
    close(master);
}

int main(void)
{
    const char *slave_name;
    struct emulator emulator;
    const char push = PUSH_COMMAND;
    freq_t freq = 0;
    int control[2];
    int master, guard, status;
    pthread_t thread;
    RIG *rig;
    int failures = 0;

    master = posix_openpt(O_RDWR | O_NOCTTY);

    if (master < 0 || grantpt(master) < 0 || unlockpt(master) < 0)
    {
        perror("posix_openpt");
        return 1;
    }

    slave_name = ptsname(master);

    /* Keeps the slave open so the master never sees a hangup while the rig's
     * port is closed */
    if (slave_name == NULL || (guard = open(slave_name, O_RDWR | O_NOCTTY)) < 0)
    {
        perror("open pty slave");
        close(master);
        return 1;
    }

    if (pipe(control) < 0)
    {
        perror("pipe");
        close(guard);
        close(master);
        return 1;
    }

    emulator.fd = master;
    emulator.control_fd = control[0];

    if (pthread_create(&thread, NULL, emulate, &emulator) != 0)
    {
        fprintf(stderr, "pthread_create failed\n");
        close(control[0]);
        close(control[1]);
        close(guard);
        close(master);
        return 1;
    }

    rig_set_debug_level(RIG_DEBUG_NONE);
    rig_load_backend("icom");
    rig = rig_init(RIG_MODEL_IC7610);
    failures += expect(rig != NULL, "initialize IC-7610 backend");

    if (rig == NULL)
    {
        stop_emulator(thread, control, guard, master);
        return 1;
    }

    rig_set_conf(rig, rig_token_lookup(rig, "rig_pathname"), slave_name);
    rig_set_conf(rig, rig_token_lookup(rig, "async"), "1");
    status = rig_open(rig);
    failures += expect(status == RIG_OK,
                       "open IC-7610 emulator with async data enabled");

    if (status == RIG_OK)
    {
        rig_set_freq_callback(rig, freq_event, NULL);
        rig_set_spectrum_callback(rig, spectrum_event, NULL);

        if (write(control[1], &push, 1) != 1)
        {
            perror("write control pipe");
            exit(2);
        }

        wait_for_events();
        failures += check_events();

        /* The frequency event updated the cache; read the rig instead */
        rig_set_cache_timeout_ms(rig, HAMLIB_CACHE_ALL, 0);
        failures += expect(rig_get_freq(rig, RIG_VFO_CURR, &freq) == RIG_OK
                           && freq == SOLICITED_FREQ,
                           "read the frequency through the sync data pipe");
        rig_close(rig);
    }

    rig_cleanup(rig);
    stop_emulator(thread, control, guard, master);

    return failures == 0 ? 0 : 1;
}

#endif
