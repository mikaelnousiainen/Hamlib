/*
 * Hamlib dummy rig's memory to VFO operation
 * Copyright (c) 2026 by Hamlib Team
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

/*
 * TO_VFO loads the current memory channel into the VFO. With a VFO selected,
 * the VFO takes the channel's frequency, mode and width, and the channel is
 * left as it is; on another VFO than the current one, that VFO alone does.
 *
 * Every read goes to the rig: the cache timeout is 0.
 */

#include <stdarg.h>
#include <stdio.h>

#include "hamlib/rig.h"
#include "hamlib/riglist.h"

#define MEM_CHANNEL 1
#define MEM_FREQ 7074000

static int failed;

static void check(int condition, const char *format, ...)
{
    va_list args;

    if (condition)
    {
        return;
    }

    va_start(args, format);
    fprintf(stderr, "FAIL: ");
    vfprintf(stderr, format, args);
    fprintf(stderr, "\n");
    va_end(args);
    failed = 1;
}

static freq_t read_freq(RIG *rig, vfo_t vfo)
{
    freq_t freq = 0;
    int status = rig_get_freq(rig, vfo, &freq);

    check(status == RIG_OK, "reading %s's frequency: %d", rig_strvfo(vfo),
          status);
    return freq;
}

static void tune(RIG *rig, vfo_t vfo, freq_t freq, rmode_t mode,
                 pbwidth_t width)
{
    check(rig_set_freq(rig, vfo, freq) == RIG_OK
          && rig_set_mode(rig, vfo, mode, width) == RIG_OK,
          "setting %s to %.0f %s", rig_strvfo(vfo), freq, rig_strrmode(mode));
}

static void op(RIG *rig, vfo_t vfo, vfo_op_t vfo_op)
{
    int status = rig_vfo_op(rig, vfo, vfo_op);

    check(status == RIG_OK, "%s on %s: %d", rig_strvfop(vfo_op),
          rig_strvfo(vfo), status);
}

int main(void)
{
    RIG *rig;
    rmode_t mem_mode, mode;
    pbwidth_t mem_width, width;
    freq_t freq;

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

    rig_set_cache_timeout_ms(rig, HAMLIB_CACHE_ALL, 0);
    check(rig_set_vfo(rig, RIG_VFO_A) == RIG_OK, "selecting VFO A");

    /* The channel holds MEM_FREQ in CW, stored from VFO A */
    tune(rig, RIG_VFO_A, MEM_FREQ, RIG_MODE_CW, 500);
    check(rig_get_mode(rig, RIG_VFO_A, &mem_mode, &mem_width) == RIG_OK,
          "reading VFO A's mode");
    check(rig_set_mem(rig, RIG_VFO_CURR, MEM_CHANNEL) == RIG_OK,
          "selecting memory channel %d", MEM_CHANNEL);
    op(rig, RIG_VFO_CURR, RIG_OP_FROM_VFO);

    /* VFO A moves on and loads the channel */
    tune(rig, RIG_VFO_A, 14074000, RIG_MODE_USB, 2400);
    op(rig, RIG_VFO_CURR, RIG_OP_TO_VFO);
    freq = read_freq(rig, RIG_VFO_A);
    check(rig_get_mode(rig, RIG_VFO_A, &mode, &width) == RIG_OK,
          "reading VFO A's mode");
    check(freq == MEM_FREQ && mode == mem_mode && width == mem_width,
          "TO_VFO on VFO A: %.0f %s %ld, want %d %s %ld", freq,
          rig_strrmode(mode), (long)width, MEM_FREQ, rig_strrmode(mem_mode),
          (long)mem_width);

    /* The channel is left as it was: VFO A, moved on again, loads it again */
    tune(rig, RIG_VFO_A, 21074000, RIG_MODE_USB, 2400);
    op(rig, RIG_VFO_CURR, RIG_OP_TO_VFO);
    freq = read_freq(rig, RIG_VFO_A);
    check(freq == MEM_FREQ, "TO_VFO on VFO A again: %.0f, want %d", freq,
          MEM_FREQ);

    /* On VFO B, VFO A left as it is */
    tune(rig, RIG_VFO_A, 3573000, RIG_MODE_USB, 2400);
    tune(rig, RIG_VFO_B, 28074000, RIG_MODE_USB, 2400);
    check(rig_set_mem(rig, RIG_VFO_B, MEM_CHANNEL) == RIG_OK,
          "selecting memory channel %d on VFO B", MEM_CHANNEL);
    op(rig, RIG_VFO_B, RIG_OP_TO_VFO);
    freq = read_freq(rig, RIG_VFO_B);
    check(freq == MEM_FREQ, "TO_VFO on VFO B: %.0f, want %d", freq, MEM_FREQ);
    freq = read_freq(rig, RIG_VFO_A);
    check(freq == 3573000, "VFO A after TO_VFO on VFO B: %.0f, want 3573000",
          freq);

    rig_close(rig);
    rig_cleanup(rig);

    if (failed)
    {
        return 1;
    }

    printf("the dummy rig's TO_VFO loads the memory channel into the VFO: OK\n");
    return 0;
}
