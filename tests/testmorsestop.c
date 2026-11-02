/*
 * Hamlib Morse data handler stopped while it keys the rig
 * Copyright (c) 2026 by Hamlib Team
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

/*
 * The Morse data handler holds the API lock while it sends. rig_close() stops
 * the handler, and a handler stopped in the middle of a send must not leave
 * the lock held, or the next call that takes it waits for ever.
 *
 * The dummy rig's send_morse is replaced by one that keys for a second. A
 * character is queued and, once the handler keys it, the rig is closed: the
 * send must be finished, not cut short, and another thread must then take the
 * lock at once. With a send that keys for half a minute, longer than
 * rig_close() waits for the handler, the handler is cancelled in its send,
 * and the lock must be free all the same. Either way the rig, opened again,
 * must be read, not answered from the cache as while a send is under way.
 */

#include <pthread.h>
#include <stdio.h>
#include <sys/time.h>

#include "hamlib/rig.h"
#include "hamlib/riglist.h"

static volatile int keying;   /* the handler is in send_morse */
static volatile int finished; /* the sends that keyed to their end */
static volatile int locked;   /* the other thread took the lock */
static volatile int reads;    /* the frequency reads that reached the rig */
static int send_ms;           /* how long a send keys */

static int get(volatile int *value)
{
    return __atomic_load_n(value, __ATOMIC_SEQ_CST);
}

static void set(volatile int *value, int to)
{
    __atomic_store_n(value, to, __ATOMIC_SEQ_CST);
}

static int slow_send_morse(RIG *rig, vfo_t vfo, const char *msg)
{
    int i;

    (void)rig;
    (void)vfo;
    (void)msg;
    set(&keying, 1);

    for (i = 0; i < send_ms / 100; i++)
    {
        hl_usleep(100 * 1000);
    }

    set(&finished, get(&finished) + 1);
    set(&keying, 0);
    return RIG_OK;
}

static int cw_get_mode(RIG *rig, vfo_t vfo, rmode_t *mode, pbwidth_t *width)
{
    (void)rig;
    (void)vfo;
    *mode = RIG_MODE_CW;
    *width = 500;
    return RIG_OK;
}

static int counted_get_freq(RIG *rig, vfo_t vfo, freq_t *freq)
{
    (void)rig;
    (void)vfo;
    *freq = 14074000;
    set(&reads, get(&reads) + 1);
    return RIG_OK;
}

static void *take_lock(void *arg)
{
    RIG *rig = arg;

    rig_lock(rig, 1);
    set(&locked, 1);
    rig_lock(rig, 0);
    return NULL;
}

/* Waits up to the given milliseconds for the value to become the one wanted */
static int wait_for(volatile int *value, int wanted, int ms)
{
    int i;

    for (i = 0; i < ms && get(value) != wanted; i++)
    {
        hl_usleep(1000);
    }

    return get(value) == wanted;
}

static double now_ms(void)
{
    struct timeval tv;

    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

/*
 * Closes the rig while its handler keys a send of the given length: the close
 * must take from min_ms to max_ms, the send end or be cut short as wanted,
 * the lock be free afterwards, and the rig, opened again, be read
 */
static int run_case(const char *name, int ms, int want_finished,
                    double min_ms, double max_ms)
{
    RIG *rig;
    pthread_t thread;
    value_t wpm;
    freq_t freq;
    double started, took;
    int failures = 0;
    int (*dummy_get_freq)(RIG *rig, vfo_t vfo, freq_t *freq);

    send_ms = ms;
    set(&keying, 0);
    set(&finished, 0);
    set(&locked, 0);
    rig = rig_init(RIG_MODEL_DUMMY);

    if (rig == NULL)
    {
        printf("%s: failed to initialize the Dummy rig\n", name);
        return 1;
    }

    rig->caps->send_morse = slow_send_morse;
    rig->caps->get_mode = cw_get_mode;

    if (rig_open(rig) != RIG_OK)
    {
        printf("%s: failed to open the Dummy rig\n", name);
        return 1;
    }

    /* fast enough that rig_close() waits its shortest, 10 s */
    wpm.i = 200;
    rig_set_level(rig, RIG_VFO_CURR, RIG_LEVEL_KEYSPD, wpm);

    if (rig_send_morse(rig, RIG_VFO_CURR, "E") != RIG_OK
            || !wait_for(&keying, 1, 2000))
    {
        printf("%s: the Morse data handler did not key the queued text\n", name);
        return 1;
    }

    started = now_ms();
    rig_close(rig);
    took = now_ms() - started;

    if (get(&finished) != want_finished || took < min_ms || took > max_ms)
    {
        printf("FAIL: %s: closing the rig took %.0f ms and %s the send, want %.0f to %.0f ms and %s\n",
               name, took, get(&finished) ? "finished" : "cut short", min_ms, max_ms,
               want_finished ? "finished" : "cut short");
        failures++;
    }
    else
    {
        printf("PASS: %s: closing the rig took %.0f ms and %s the send\n", name, took,
               want_finished ? "finished" : "cut short");
    }

    if (pthread_create(&thread, NULL, take_lock, rig) != 0)
    {
        printf("%s: failed to start a thread\n", name);
        return 1;
    }

    if (!wait_for(&locked, 1, 2000))
    {
        /* the thread waits for a lock nobody will release: leave it */
        printf("FAIL: %s: the Morse data handler, stopped, left the API lock held\n", name);
        return 1;
    }

    pthread_join(thread, NULL);
    printf("PASS: %s: the API lock is free once the rig is closed\n", name);

    if (rig_open(rig) != RIG_OK)
    {
        printf("%s: failed to open the Dummy rig again\n", name);
        return 1;
    }

    rig_set_cache_timeout_ms(rig, HAMLIB_CACHE_ALL, 0);
    dummy_get_freq = rig->caps->get_freq;
    rig->caps->get_freq = counted_get_freq;
    set(&reads, 0);
    rig_get_freq(rig, RIG_VFO_CURR, &freq);
    rig->caps->get_freq = dummy_get_freq;

    if (get(&reads) != 1)
    {
        printf("FAIL: %s: opened again, the rig answered from the cache, as while a send is under way\n",
               name);
        failures++;
    }
    else
    {
        printf("PASS: %s: opened again, the rig is read\n", name);
    }

    rig_close(rig);
    rig_cleanup(rig);
    return failures;
}

int main(void)
{
    int failures = 0;

    rig_set_debug(RIG_DEBUG_NONE);
    setvbuf(stdout, NULL, _IOLBF, 0);

    if (rig_load_backend("dummy") != RIG_OK)
    {
        printf("failed to load the dummy backend\n");
        return 1;
    }

    failures += run_case("a send of a second", 1000, 1, 500, 3000);
    failures += run_case("a send of half a minute", 30000, 0, 9500, 13000);

    return failures > 0;
}
