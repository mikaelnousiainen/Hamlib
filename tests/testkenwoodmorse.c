/*
 * Hamlib Kenwood Morse sending while the rig's buffer stays full
 * Copyright (c) 2026 by Hamlib Team
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

/*
 * kenwood_send_morse() asks "KY;" whether the rig has room for more text and
 * waits while it answers KY1. A rig that answers KY1 for ever must not keep
 * the send, and the API lock the Morse data handler holds for it, for ever:
 * the wait ends after the time the rig takes to key a full buffer at its
 * keyer speed, and the send fails with -RIG_ETIMEOUT.
 *
 * An emulated TS-590S over a pseudo terminal reports a keyer speed of 999 WPM,
 * which makes that time about 1.3 s, and answers KY1 for ever: the send must
 * fail with -RIG_ETIMEOUT after it, not before. Answering KY1 three times
 * and then KY0, it must send the text and succeed.
 */

#if defined(_WIN32) || defined(WIN32)

int main(void)
{
    return 77;
}

#else

#define _XOPEN_SOURCE 600

#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#include "hamlib/rig.h"
#include "hamlib/riglist.h"

/* 1,267,200 ms / 999 WPM: the longest the send waits for room */
#define WAIT_MS 1268

/* The emulated rig: answers KY1 that many times before KY0, or for ever when negative; tells the keyed text */
static int emulate(int fd, int full_answers, int keyed)
{
    char cmd[64], reply[80];
    size_t used = 0;
    char byte;

    while (read(fd, &byte, 1) == 1)
    {
        if (byte != ';')
        {
            if (used + 1 >= sizeof(cmd)) { return 1; }

            cmd[used++] = byte;
            continue;
        }

        cmd[used] = 0;
        used = 0;
        reply[0] = 0;

        if (!strcmp(cmd, "PS")) { strcpy(reply, "PS1;"); }
        else if (!strcmp(cmd, "ID")) { strcpy(reply, "ID021;"); }
        else if (!strcmp(cmd, "FV")) { strcpy(reply, "FV1.04;"); }
        else if (!strcmp(cmd, "IF"))
        {
            strcpy(reply, "IF00014074000     +000000000030000080;");
        }
        else if (!strcmp(cmd, "FA")) { strcpy(reply, "FA00014074000;"); }
        else if (!strcmp(cmd, "KS")) { strcpy(reply, "KS999;"); }
        else if (!strcmp(cmd, "KY"))
        {
            if (full_answers != 0)
            {
                strcpy(reply, "KY1;");

                if (full_answers > 0) { full_answers--; }
            }
            else
            {
                strcpy(reply, "KY0;");
            }
        }
        else if (!strncmp(cmd, "KY ", 3))
        {
            if (write(keyed, "k", 1) != 1) { return 1; }
        }
        else
        {
            strcpy(reply, "?;");
        }

        if (reply[0] && write(fd, reply, strlen(reply)) < 0) { return 1; }
    }

    return 0;
}

struct send
{
    RIG *rig;
    volatile int done;
    int retval;
};

static void *send_text(void *arg)
{
    struct send *s = arg;

    s->retval = s->rig->caps->send_morse(s->rig, RIG_VFO_CURR, "TEST");
    __atomic_store_n(&s->done, 1, __ATOMIC_SEQ_CST);
    return NULL;
}

/* A status as rigerror2() words it, without its line end */
static const char *status_text(int status)
{
    static char text[2][128];
    static int which;
    char *end;

    which = !which;
    snprintf(text[which], sizeof(text[which]), "%s", rigerror2(status));
    end = strchr(text[which], '\n');

    if (end) { *end = '\0'; }

    return text[which];
}

static double now_ms(void)
{
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

/* Opens the emulated rig answering KY1 that many times, sends the text and checks how the send ended */
static int run_case(const char *name, int full_answers, int want_retval,
                    double min_ms, double max_ms)
{
    int master, slave_check, status, result = 1, keyed[2], texts = 0;
    char k;
    pid_t child;
    const char *path;
    RIG *rig;
    struct send s = { 0 };
    pthread_t thread;
    double started, took;
    int i;

    master = posix_openpt(O_RDWR | O_NOCTTY);

    if (master < 0 || grantpt(master) < 0 || unlockpt(master) < 0
            || (path = ptsname(master)) == NULL)
    {
        printf("%s: failed to make a pseudo terminal\n", name);
        return 1;
    }

    /* kept open so the terminal stays while the rig is closed */
    slave_check = open(path, O_RDWR | O_NOCTTY);

    if (pipe(keyed) != 0)
    {
        printf("%s: failed to make a pipe\n", name);
        return 1;
    }

    child = fork();

    if (child == 0)
    {
        _exit(emulate(master, full_answers, keyed[1]));
    }

    fcntl(keyed[0], F_SETFL, O_NONBLOCK);

    rig = rig_init(RIG_MODEL_TS590S);

    if (rig == NULL)
    {
        printf("%s: failed to initialize the TS-590S\n", name);
        goto cleanup;
    }

    rig_set_conf(rig, rig_token_lookup(rig, "rig_pathname"), path);
    rig_set_conf(rig, rig_token_lookup(rig, "cache_timeout"), "0");

    if (rig_open(rig) != RIG_OK)
    {
        printf("%s: failed to open the emulated TS-590S\n", name);
        rig_cleanup(rig);
        goto cleanup;
    }

    s.rig = rig;
    started = now_ms();

    if (pthread_create(&thread, NULL, send_text, &s) != 0)
    {
        printf("%s: failed to start a thread\n", name);
        goto cleanup;
    }

    for (i = 0; i < 10000 && !__atomic_load_n(&s.done, __ATOMIC_SEQ_CST); i++)
    {
        hl_usleep(1000);
    }

    took = now_ms() - started;

    if (!__atomic_load_n(&s.done, __ATOMIC_SEQ_CST))
    {
        /* the thread waits for a rig that never makes room: leave it */
        printf("FAIL: %s: the send did not end in 10 s\n", name);
        kill(child, SIGKILL);
        return 1;
    }

    pthread_join(thread, NULL);
    /* the text reaches the rig after the wait, or never when the wait fails */
    hl_usleep(100 * 1000);

    while (read(keyed[0], &k, 1) == 1)
    {
        texts++;
    }

    if (s.retval != want_retval || took < min_ms || took > max_ms
            || (texts > 0) != (want_retval == RIG_OK))
    {
        printf("FAIL: %s: the send ended with %s after %.0f ms, the text sent %d times; "
               "want %s in %.0f to %.0f ms, the text sent %s\n", name, status_text(s.retval), took,
               texts, status_text(want_retval), min_ms, max_ms,
               want_retval == RIG_OK ? "once" : "never");
    }
    else
    {
        printf("PASS: %s: the send ended with %s after %.0f ms\n", name,
               status_text(s.retval), took);
        result = 0;
    }

    rig_close(rig);
    rig_cleanup(rig);

cleanup:
    kill(child, SIGKILL);
    waitpid(child, &status, 0);
    close(keyed[0]);
    close(keyed[1]);
    close(slave_check);
    close(master);
    return result;
}

int main(void)
{
    int failures = 0;

    rig_set_debug(RIG_DEBUG_NONE);
    setvbuf(stdout, NULL, _IOLBF, 0);

    if (rig_load_backend("kenwood") != RIG_OK)
    {
        printf("failed to load the Kenwood backend\n");
        return 1;
    }

    failures += run_case("a buffer that stays full", -1, -RIG_ETIMEOUT, WAIT_MS,
                         WAIT_MS + 2000);
    failures += run_case("a buffer full for three answers", 3, RIG_OK, 0, 2000);

    return failures > 0;
}

#endif
