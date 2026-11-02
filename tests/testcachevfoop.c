/*
 * Hamlib frequency cache after a VFO operation
 * Copyright (c) 2026 by Hamlib Team
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

/*
 * A VFO operation can tune the rig without a frequency being set: exchanging
 * or copying the VFOs, or loading a memory channel into a VFO, changes what a
 * VFO is tuned to. A frequency read made right after the operation must go to
 * the rig and return the frequency the operation left, not the one cached
 * before it. So must a read after an operation whose reply was lost, which the
 * rig may have carried out all the same.
 *
 * Each case uses the dummy rig with VFO A and VFO B on different frequencies,
 * both read into the cache, whose timeout is long enough to keep them fresh
 * for the whole case.
 */

#include <stdio.h>

#include "hamlib/rig.h"
#include "hamlib/riglist.h"

#define FREQ_A 7074000
#define FREQ_B 14074000
#define FREQ_MEM 3573000
#define MEM_CHANNEL 1

static int failed;

static int (*dummy_vfo_op)(RIG *rig, vfo_t vfo, vfo_op_t op);

/* Carries the operation out, as the dummy rig does, and then loses the reply */
static int vfo_op_reply_lost(RIG *rig, vfo_t vfo, vfo_op_t op)
{
    dummy_vfo_op(rig, vfo, op);
    return -RIG_ETIMEOUT;
}

static void expect_freq(RIG *rig, vfo_t vfo, freq_t want, const char *after)
{
    freq_t freq = 0;
    int status = rig_get_freq(rig, vfo, &freq);

    if (status != RIG_OK || freq != want)
    {
        fprintf(stderr, "FAIL: %s, %s reads %.0f, want %.0f: %s", after,
                rig_strvfo(vfo), freq, want, rigerror2(status));
        failed = 1;
    }
}

static void expect_status(int status, int want, const char *what)
{
    if (status != want)
    {
        fprintf(stderr, "FAIL: %s returned %d, want %d: %s", what, status, want,
                rigerror2(status));
        failed = 1;
    }
}

/* Memory channel MEM_CHANNEL holds FREQ_MEM, written from VFO A, and is the
 * channel VFO A selects */
static int store_memory(RIG *rig)
{
    if (rig_set_vfo(rig, RIG_VFO_A) != RIG_OK
            || rig_set_freq(rig, RIG_VFO_A, FREQ_MEM) != RIG_OK
            || rig_set_mem(rig, RIG_VFO_CURR, MEM_CHANNEL) != RIG_OK
            || rig_vfo_op(rig, RIG_VFO_CURR, RIG_OP_FROM_VFO) != RIG_OK)
    {
        fprintf(stderr, "FAIL: storing %d in memory channel %d\n", FREQ_MEM,
                MEM_CHANNEL);
        failed = 1;
        return -1;
    }

    return 0;
}

/* VFO A current, on FREQ_A, VFO B on FREQ_B, and both fresh in the cache */
static int prime(RIG *rig)
{
    freq_t freq_a = 0, freq_b = 0;

    if (rig_set_vfo(rig, RIG_VFO_A) != RIG_OK
            || rig_set_freq(rig, RIG_VFO_A, FREQ_A) != RIG_OK
            || rig_set_freq(rig, RIG_VFO_B, FREQ_B) != RIG_OK
            || rig_get_freq(rig, RIG_VFO_A, &freq_a) != RIG_OK
            || rig_get_freq(rig, RIG_VFO_B, &freq_b) != RIG_OK
            || freq_a != FREQ_A || freq_b != FREQ_B)
    {
        fprintf(stderr, "FAIL: setting VFO A to %d and VFO B to %d\n", FREQ_A,
                FREQ_B);
        failed = 1;
        return -1;
    }

    return 0;
}

int main(void)
{
    RIG *rig;

    rig_set_debug(RIG_DEBUG_NONE);

    if (rig_load_backend("dummy") != RIG_OK)
    {
        fprintf(stderr, "failed to load the dummy backend\n");
        return 1;
    }

    rig = rig_init(RIG_MODEL_DUMMY);

    if (rig == NULL || rig_open(rig) != RIG_OK)
    {
        fprintf(stderr, "failed to open the Dummy rig\n");
        return 1;
    }

    rig_set_cache_timeout_ms(rig, HAMLIB_CACHE_ALL, 5000);

    if (prime(rig) == 0)
    {
        expect_status(rig_vfo_op(rig, RIG_VFO_CURR, RIG_OP_XCHG), RIG_OK, "XCHG");
        expect_freq(rig, RIG_VFO_A, FREQ_B, "after XCHG");
        expect_freq(rig, RIG_VFO_B, FREQ_A, "after XCHG");
    }

    if (prime(rig) == 0)
    {
        expect_status(rig_vfo_op(rig, RIG_VFO_CURR, RIG_OP_CPY), RIG_OK,
                      "CPY on VFO A");
        expect_freq(rig, RIG_VFO_B, FREQ_A, "after CPY on VFO A");
    }

    /* On a VFO other than the current one: VFO B selected, copied, and VFO A
     * selected again */
    if (prime(rig) == 0)
    {
        expect_status(rig_vfo_op(rig, RIG_VFO_B, RIG_OP_CPY), RIG_OK,
                      "CPY on VFO B");
        expect_freq(rig, RIG_VFO_A, FREQ_B, "after CPY on VFO B");
    }

    /* Memory selected, loaded into VFO A, and VFO A selected again */
    if (store_memory(rig) == 0 && prime(rig) == 0)
    {
        expect_status(rig_vfo_op(rig, RIG_VFO_MEM, RIG_OP_TO_VFO), RIG_OK,
                      "TO_VFO on the memory");
        expect_freq(rig, RIG_VFO_A, FREQ_MEM, "after TO_VFO on the memory");
    }

    dummy_vfo_op = rig->caps->vfo_op;
    rig->caps->vfo_op = vfo_op_reply_lost;

    if (prime(rig) == 0)
    {
        expect_status(rig_vfo_op(rig, RIG_VFO_CURR, RIG_OP_XCHG), -RIG_ETIMEOUT,
                      "XCHG whose reply is lost");
        expect_freq(rig, RIG_VFO_A, FREQ_B, "after XCHG whose reply is lost");
    }

    if (prime(rig) == 0)
    {
        expect_status(rig_vfo_op(rig, RIG_VFO_B, RIG_OP_CPY), -RIG_ETIMEOUT,
                      "CPY on VFO B whose reply is lost");
        expect_freq(rig, RIG_VFO_A, FREQ_B,
                    "after CPY on VFO B whose reply is lost");
    }

    rig->caps->vfo_op = dummy_vfo_op;

    rig_close(rig);
    rig_cleanup(rig);

    if (failed)
    {
        return 1;
    }

    printf("a read after a VFO operation returns what the rig is tuned to: OK\n");
    return 0;
}
