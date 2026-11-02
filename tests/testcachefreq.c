/*
 * Hamlib frequency cache after a failed read
 * Copyright (c) 2026 by Hamlib Team
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

/*
 * A frequency read that fails must not leave the cached frequency looking fresh: the next read goes
 * to the rig again and fails too, instead of answering RIG_OK from the cache. A rig switched off
 * that answers every read with an error would otherwise seem to answer for a cache timeout after
 * each failure.
 */

#include <stdio.h>

#include "hamlib/rig.h"
#include "hamlib/riglist.h"

static int failing_reads;

static int failing_get_freq(RIG *rig, vfo_t vfo, freq_t *freq)
{
    (void)rig;
    (void)vfo;
    (void)freq;
    failing_reads++;
    return -RIG_ERJCTED;
}

int main(void)
{
    RIG *rig;
    freq_t freq;
    int status;
    int result = 1;

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

    rig_set_cache_timeout_ms(rig, HAMLIB_CACHE_ALL, 50);

    if (rig_set_freq(rig, RIG_VFO_CURR, 14074000) != RIG_OK
            || rig_get_freq(rig, RIG_VFO_CURR, &freq) != RIG_OK)
    {
        fprintf(stderr, "failed to set and read the frequency\n");
        goto cleanup;
    }

    /* The rig stops answering, and the cached frequency has aged out */
    rig->caps->get_freq = failing_get_freq;
    hl_usleep(80 * 1000);

    status = rig_get_freq(rig, RIG_VFO_CURR, &freq);

    if (status != -RIG_ERJCTED)
    {
        fprintf(stderr, "first read after the rig stopped answering: %s, want %s\n",
                rigerror2(status), rigerror2(-RIG_ERJCTED));
        goto cleanup;
    }

    status = rig_get_freq(rig, RIG_VFO_CURR, &freq);

    if (status != -RIG_ERJCTED || failing_reads != 2)
    {
        fprintf(stderr,
                "second read right after a failed one: %s after %d reads of the rig, want %s after 2\n",
                rigerror2(status), failing_reads, rigerror2(-RIG_ERJCTED));
        goto cleanup;
    }

    printf("a failed frequency read leaves the cache stale: OK\n");
    result = 0;

cleanup:
    rig_close(rig);
    rig_cleanup(rig);
    return result;
}
