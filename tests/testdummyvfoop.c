/*
 * Hamlib dummy rig's tuning operations
 * Copyright (c) 2026 by Hamlib Team
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

/*
 * The dummy rig's UP and DOWN step the VFO's frequency by its tuning step.
 * BAND_UP and BAND_DOWN step through 13 bands, 160 m to 70 cm, wrapping at
 * both ends; from a frequency outside every band they go to the next band
 * above or below it. A band stack shared by the VFOs keeps what a VFO was
 * tuned to when it left a band, frequency, mode and width, and a return to
 * that band restores it; a first visit lands on the band's first frequency,
 * the mode unchanged. Each operation acts on the VFO it is given.
 *
 * In memory mode the current memory channel's frequency is read and set, and
 * UP and DOWN step it. An operation whose read of the rig fails returns that
 * read's error, as on the Dummy No VFO model, which takes no current VFO.
 *
 * Every read goes to the rig: the cache timeout is 0.
 */

#include <stdarg.h>
#include <stdio.h>

#include "hamlib/rig.h"
#include "hamlib/riglist.h"

#define NBANDS 13

/* Each band's first frequency, from 160 m up to 70 cm */
static const freq_t first_freq[NBANDS] =
{
    1810000, 3500000, 5351500, 7000000, 10100000, 14000000, 18068000,
    21000000, 24890000, 28000000, 50000000, 144000000, 430000000
};

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

static RIG *open_model(rig_model_t model)
{
    RIG *rig = rig_init(model);

    if (rig == NULL || rig_open(rig) != RIG_OK)
    {
        fprintf(stderr, "failed to open the dummy rig model %u\n", model);
        return NULL;
    }

    rig_set_cache_timeout_ms(rig, HAMLIB_CACHE_ALL, 0);
    return rig;
}

static RIG *open_dummy(void)
{
    RIG *rig = open_model(RIG_MODEL_DUMMY);

    if (rig != NULL)
    {
        rig_set_vfo(rig, RIG_VFO_A);
    }

    return rig;
}

static void close_dummy(RIG *rig)
{
    rig_close(rig);
    rig_cleanup(rig);
}

static freq_t read_freq(RIG *rig, vfo_t vfo)
{
    freq_t freq = 0;
    int status = rig_get_freq(rig, vfo, &freq);

    check(status == RIG_OK, "reading %s's frequency: %d", rig_strvfo(vfo),
          status);
    return freq;
}

static void read_mode(RIG *rig, vfo_t vfo, rmode_t *mode, pbwidth_t *width)
{
    int status = rig_get_mode(rig, vfo, mode, width);

    check(status == RIG_OK, "reading %s's mode: %d", rig_strvfo(vfo), status);
}

static void tune(RIG *rig, vfo_t vfo, freq_t freq)
{
    int status = rig_set_freq(rig, vfo, freq);

    check(status == RIG_OK, "setting %s to %.0f: %d", rig_strvfo(vfo), freq,
          status);
}

static void op(RIG *rig, vfo_t vfo, vfo_op_t vfo_op)
{
    int status = rig_vfo_op(rig, vfo, vfo_op);

    check(status == RIG_OK, "%s on %s: %d", rig_strvfop(vfo_op),
          rig_strvfo(vfo), status);
}

/* From freq, one band operation on VFO A must land on want */
static void expect_band_op(RIG *rig, freq_t freq, vfo_op_t vfo_op,
                           freq_t want)
{
    freq_t got;

    tune(rig, RIG_VFO_A, freq);
    op(rig, RIG_VFO_CURR, vfo_op);
    got = read_freq(rig, RIG_VFO_A);
    check(got == want, "%s from %.0f: %.0f, want %.0f", rig_strvfop(vfo_op),
          freq, got, want);
}

static void test_up_down(void)
{
    RIG *rig = open_dummy();
    freq_t freq;

    if (rig == NULL)
    {
        failed = 1;
        return;
    }

    /* The dummy rig's tuning step is 0 until one is set */
    check(rig_set_ts(rig, RIG_VFO_A, 500) == RIG_OK, "setting VFO A's step");
    tune(rig, RIG_VFO_A, 14074000);

    op(rig, RIG_VFO_CURR, RIG_OP_UP);
    freq = read_freq(rig, RIG_VFO_A);
    check(freq == 14074500, "UP from 14074000 by 500: %.0f", freq);

    op(rig, RIG_VFO_CURR, RIG_OP_DOWN);
    op(rig, RIG_VFO_CURR, RIG_OP_DOWN);
    freq = read_freq(rig, RIG_VFO_A);
    check(freq == 14073500, "DOWN twice from 14074500 by 500: %.0f", freq);

    /* On VFO B, by VFO B's own step, VFO A left as it is */
    check(rig_set_ts(rig, RIG_VFO_B, 1000) == RIG_OK, "setting VFO B's step");
    tune(rig, RIG_VFO_B, 7074000);
    op(rig, RIG_VFO_B, RIG_OP_UP);
    freq = read_freq(rig, RIG_VFO_B);
    check(freq == 7075000, "UP on VFO B from 7074000 by 1000: %.0f", freq);
    freq = read_freq(rig, RIG_VFO_A);
    check(freq == 14073500, "VFO A after UP on VFO B: %.0f", freq);

    close_dummy(rig);
}

/* Up through every band from 160 m and back to it, each a first visit that
 * keeps the mode, then down through every band from 70 cm and back to it */
static void test_band_order(void)
{
    RIG *rig = open_dummy();
    rmode_t mode;
    pbwidth_t width;
    freq_t freq;

    if (rig == NULL)
    {
        failed = 1;
        return;
    }

    check(rig_set_mode(rig, RIG_VFO_A, RIG_MODE_CW, 500) == RIG_OK,
          "setting VFO A to CW");
    tune(rig, RIG_VFO_A, 1850000);

    for (int band = 1; band < NBANDS; band++)
    {
        op(rig, RIG_VFO_CURR, RIG_OP_BAND_UP);
        freq = read_freq(rig, RIG_VFO_A);
        read_mode(rig, RIG_VFO_A, &mode, &width);
        check(freq == first_freq[band], "BAND_UP to band %d: %.0f, want %.0f",
              band, freq, first_freq[band]);
        check(mode == RIG_MODE_CW, "BAND_UP to band %d: mode %s, want CW", band,
              rig_strrmode(mode));
    }

    /* From 70 cm up to 160 m, where VFO A was */
    op(rig, RIG_VFO_CURR, RIG_OP_BAND_UP);
    freq = read_freq(rig, RIG_VFO_A);
    check(freq == 1850000, "BAND_UP from 70 cm: %.0f, want 1850000", freq);

    close_dummy(rig);

    rig = open_dummy();

    if (rig == NULL)
    {
        failed = 1;
        return;
    }

    tune(rig, RIG_VFO_A, 432100000);

    for (int band = NBANDS - 2; band >= 0; band--)
    {
        op(rig, RIG_VFO_CURR, RIG_OP_BAND_DOWN);
        freq = read_freq(rig, RIG_VFO_A);
        check(freq == first_freq[band], "BAND_DOWN to band %d: %.0f, want %.0f",
              band, freq, first_freq[band]);
    }

    /* From 160 m down to 70 cm, where VFO A was */
    op(rig, RIG_VFO_CURR, RIG_OP_BAND_DOWN);
    freq = read_freq(rig, RIG_VFO_A);
    check(freq == 432100000, "BAND_DOWN from 160 m: %.0f, want 432100000",
          freq);

    close_dummy(rig);
}

/* From a frequency outside every band, to the next band above or below it,
 * wrapping beyond 70 cm and below 160 m */
static void test_outside_bands(void)
{
    RIG *rig = open_dummy();

    if (rig == NULL)
    {
        failed = 1;
        return;
    }

    expect_band_op(rig, 2500000, RIG_OP_BAND_UP, 3500000);
    expect_band_op(rig, 2500000, RIG_OP_BAND_DOWN, 1810000);
    expect_band_op(rig, 6000000, RIG_OP_BAND_UP, 7000000);
    expect_band_op(rig, 6000000, RIG_OP_BAND_DOWN, 5351500);
    expect_band_op(rig, 100000000, RIG_OP_BAND_UP, 144000000);
    expect_band_op(rig, 100000000, RIG_OP_BAND_DOWN, 50000000);
    expect_band_op(rig, 1000000, RIG_OP_BAND_UP, 1810000);
    expect_band_op(rig, 1000000, RIG_OP_BAND_DOWN, 430000000);
    expect_band_op(rig, 1200000000, RIG_OP_BAND_UP, 1810000);
    expect_band_op(rig, 1200000000, RIG_OP_BAND_DOWN, 430000000);

    close_dummy(rig);
}

/* Leaving a band keeps the VFO's frequency, mode and width for it, and a
 * return restores them; the stack is shared by the VFOs */
static void test_band_stack(void)
{
    RIG *rig = open_dummy();
    rmode_t usb_mode, am_mode, mode;
    pbwidth_t usb_width, am_width, width;
    freq_t freq;

    if (rig == NULL)
    {
        failed = 1;
        return;
    }

    tune(rig, RIG_VFO_A, 14074000);
    check(rig_set_mode(rig, RIG_VFO_A, RIG_MODE_USB, 2400) == RIG_OK,
          "setting VFO A to USB");
    read_mode(rig, RIG_VFO_A, &usb_mode, &usb_width);

    /* 17 m for the first time, in USB still, then tuned and put in AM */
    op(rig, RIG_VFO_CURR, RIG_OP_BAND_UP);
    freq = read_freq(rig, RIG_VFO_A);
    read_mode(rig, RIG_VFO_A, &mode, &width);
    check(freq == 18068000 && mode == usb_mode,
          "first BAND_UP to 17 m: %.0f %s, want 18068000 %s", freq,
          rig_strrmode(mode), rig_strrmode(usb_mode));

    tune(rig, RIG_VFO_A, 18100000);
    check(rig_set_mode(rig, RIG_VFO_A, RIG_MODE_AM, 6000) == RIG_OK,
          "setting VFO A to AM");
    read_mode(rig, RIG_VFO_A, &am_mode, &am_width);

    op(rig, RIG_VFO_CURR, RIG_OP_BAND_DOWN);
    freq = read_freq(rig, RIG_VFO_A);
    read_mode(rig, RIG_VFO_A, &mode, &width);
    check(freq == 14074000 && mode == usb_mode && width == usb_width,
          "BAND_DOWN back to 20 m: %.0f %s %ld, want 14074000 %s %ld", freq,
          rig_strrmode(mode), (long)width, rig_strrmode(usb_mode),
          (long)usb_width);

    op(rig, RIG_VFO_CURR, RIG_OP_BAND_UP);
    freq = read_freq(rig, RIG_VFO_A);
    read_mode(rig, RIG_VFO_A, &mode, &width);
    check(freq == 18100000 && mode == am_mode && width == am_width,
          "BAND_UP back to 17 m: %.0f %s %ld, want 18100000 %s %ld", freq,
          rig_strrmode(mode), (long)width, rig_strrmode(am_mode),
          (long)am_width);

    /* VFO B, on 2 m, goes up to 70 cm for the first time and back to 2 m;
     * then from 15 m, with VFO B current, down to 17 m as VFO A left it */
    tune(rig, RIG_VFO_B, 145500000);
    op(rig, RIG_VFO_B, RIG_OP_BAND_UP);
    freq = read_freq(rig, RIG_VFO_B);
    check(freq == 430000000, "BAND_UP on VFO B from 2 m: %.0f", freq);
    freq = read_freq(rig, RIG_VFO_A);
    check(freq == 18100000, "VFO A after BAND_UP on VFO B: %.0f", freq);
    op(rig, RIG_VFO_B, RIG_OP_BAND_DOWN);
    freq = read_freq(rig, RIG_VFO_B);
    check(freq == 145500000, "BAND_DOWN on VFO B back to 2 m: %.0f", freq);

    check(rig_set_vfo(rig, RIG_VFO_B) == RIG_OK, "selecting VFO B");
    tune(rig, RIG_VFO_B, 21074000);
    op(rig, RIG_VFO_CURR, RIG_OP_BAND_DOWN);
    freq = read_freq(rig, RIG_VFO_B);
    read_mode(rig, RIG_VFO_B, &mode, &width);
    check(freq == 18100000 && mode == am_mode && width == am_width,
          "BAND_DOWN on VFO B from 15 m to 17 m: %.0f %s %ld, want 18100000 %s %ld",
          freq, rig_strrmode(mode), (long)width, rig_strrmode(am_mode),
          (long)am_width);

    close_dummy(rig);
}

/* In memory mode the current channel's frequency is read and set, UP and
 * DOWN step it by its own step, and the VFO it was stored from is left as it
 * is; BAND_UP and BAND_DOWN refuse the memory, whose mode the dummy rig keeps
 * no band stack for */
static void test_memory_mode(void)
{
    RIG *rig = open_dummy();
    freq_t freq;
    int status;

    if (rig == NULL)
    {
        failed = 1;
        return;
    }

    /* Channel 3 holds 7074000, stored from VFO A, which then moves on */
    tune(rig, RIG_VFO_A, 7074000);
    check(rig_set_mem(rig, RIG_VFO_CURR, 3) == RIG_OK,
          "selecting memory channel 3");
    op(rig, RIG_VFO_CURR, RIG_OP_FROM_VFO);
    tune(rig, RIG_VFO_A, 14074000);

    status = rig_set_vfo(rig, RIG_VFO_MEM);
    check(status == RIG_OK, "selecting the memory: %d", status);
    freq = read_freq(rig, RIG_VFO_CURR);
    check(freq == 7074000, "memory channel 3 reads %.0f, want 7074000", freq);

    tune(rig, RIG_VFO_CURR, 7040000);
    freq = read_freq(rig, RIG_VFO_CURR);
    check(freq == 7040000, "memory channel 3 set to 7040000 reads %.0f", freq);

    check(rig_set_ts(rig, RIG_VFO_CURR, 500) == RIG_OK,
          "setting memory channel 3's step");
    op(rig, RIG_VFO_CURR, RIG_OP_UP);
    freq = read_freq(rig, RIG_VFO_CURR);
    check(freq == 7040500, "UP on memory channel 3 by 500: %.0f", freq);
    op(rig, RIG_VFO_CURR, RIG_OP_DOWN);
    op(rig, RIG_VFO_CURR, RIG_OP_DOWN);
    freq = read_freq(rig, RIG_VFO_CURR);
    check(freq == 7039500, "DOWN twice on memory channel 3 by 500: %.0f", freq);

    status = rig_vfo_op(rig, RIG_VFO_CURR, RIG_OP_BAND_UP);
    check(status == -RIG_EVFO, "BAND_UP on the memory: %d, want %d", status,
          -RIG_EVFO);
    status = rig_vfo_op(rig, RIG_VFO_CURR, RIG_OP_BAND_DOWN);
    check(status == -RIG_EVFO, "BAND_DOWN on the memory: %d, want %d", status,
          -RIG_EVFO);
    freq = read_freq(rig, RIG_VFO_CURR);
    check(freq == 7039500, "memory channel 3 after band up and down: %.0f",
          freq);

    check(rig_set_vfo(rig, RIG_VFO_A) == RIG_OK, "selecting VFO A");
    freq = read_freq(rig, RIG_VFO_A);
    check(freq == 14074000, "VFO A after the memory's changes: %.0f", freq);

    /* Back to the memory: channel 3 as it was left */
    check(rig_set_vfo(rig, RIG_VFO_MEM) == RIG_OK, "selecting the memory again");
    freq = read_freq(rig, RIG_VFO_CURR);
    check(freq == 7039500, "memory channel 3 selected again: %.0f", freq);

    close_dummy(rig);
}

/* An operation whose read of the rig fails returns the read's error: the Dummy
 * No VFO model reads no frequency of the current VFO */
static void test_read_errors(void)
{
    static const vfo_op_t ops[] =
    {
        RIG_OP_UP, RIG_OP_DOWN, RIG_OP_BAND_UP, RIG_OP_BAND_DOWN
    };
    RIG *rig = open_model(RIG_MODEL_DUMMY_NOVFO);
    freq_t freq;

    if (rig == NULL)
    {
        failed = 1;
        return;
    }

    check(rig_set_ts(rig, RIG_VFO_CURR, 500) == RIG_OK, "setting the step");
    tune(rig, RIG_VFO_CURR, 14074000);

    for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); i++)
    {
        int status = rig_vfo_op(rig, RIG_VFO_CURR, ops[i]);

        check(status == -RIG_EINVAL, "%s on Dummy No VFO: %d, want %d",
              rig_strvfop(ops[i]), status, -RIG_EINVAL);
    }

    freq = read_freq(rig, RIG_VFO_CURR);
    check(freq == 14074000, "Dummy No VFO after the operations: %.0f", freq);

    close_dummy(rig);
}

int main(void)
{
    rig_set_debug(RIG_DEBUG_NONE);

    if (rig_load_backend("dummy") != RIG_OK)
    {
        fprintf(stderr, "failed to load the dummy backend\n");
        return 1;
    }

    test_up_down();
    test_band_order();
    test_outside_bands();
    test_band_stack();
    test_memory_mode();
    test_read_errors();

    if (failed)
    {
        return 1;
    }

    printf("the dummy rig's UP, DOWN, BAND_UP and BAND_DOWN: OK\n");
    return 0;
}
