/*
 * Hamlib band of a frequency
 * Copyright (c) 2026 by Hamlib Team
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

/*
 * rig_get_band() answers the first band of Hamlib's band table whose range
 * holds the frequency. Medium wave comes before 33 cm and 23 cm in the
 * table: frequencies of each of these bands must give their own band.
 */

#include <stdio.h>
#include <string.h>

#include "hamlib/rig.h"
#include "misc.h"

int main(void)
{
    static const struct
    {
        freq_t freq;
        hamlib_band_t band;
    } cases[] =
    {
        { 530000, (hamlib_band_t)RIG_BANDSELECT_MW },
        { 1000000, (hamlib_band_t)RIG_BANDSELECT_MW },
        { 1700000, (hamlib_band_t)RIG_BANDSELECT_MW },
        { 903000000, (hamlib_band_t)RIG_BANDSELECT_33CM },
        { 1296000000, (hamlib_band_t)RIG_BANDSELECT_23CM },
    };
    RIG rig;
    struct rig_caps caps;
    int failed = 0;

    rig_set_debug(RIG_DEBUG_NONE);

    memset(&rig, 0, sizeof(rig));
    memset(&caps, 0, sizeof(caps));
    rig.caps = &caps;

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        hamlib_band_t band = rig_get_band(&rig, cases[i].freq, -1);

        if (band != cases[i].band)
        {
            fprintf(stderr, "FAIL: %.0f Hz is in %s, want %s\n", cases[i].freq,
                    rig_get_band_str(&rig, band, 0),
                    rig_get_band_str(&rig, cases[i].band, 0));
            failed = 1;
        }
    }

    if (failed)
    {
        return 1;
    }

    printf("medium wave, 33 cm and 23 cm frequencies give their own bands: OK\n");
    return 0;
}
