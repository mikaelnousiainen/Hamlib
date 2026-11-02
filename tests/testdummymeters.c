/*
 * Reads the meters of the dummy amplifier and of the dummy rig.
 *
 * The dummy amplifier is a 1 kW amplifier on a 50 V supply. Its meters follow
 * its power state: off, every meter reads zero; on or in standby, it is idle,
 * with no power, an SWR of 1.0 and its supply at rest; in operate, it
 * transmits, its forward power set by its power level, which starts at 100 %.
 * Each meter comes from one sample of them all, taken at most every 100 ms, so
 * the readings of one poll agree with one another: reflected power with forward
 * power and SWR, drive with forward power, current with forward power and
 * voltage. The samples move a little, the temperature warms while transmitting
 * and cools while not, and FAULT and WARNING, listed in the capabilities, read
 * as empty, no fault and no warning. With static_data the meters are fixed.
 *
 * The dummy rig's SWR, ALC and temperature move within their bounds as its
 * other meters do, and are fixed with static_data.
 */

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "hamlib/rig.h"
#include "hamlib/riglist.h"
#include "hamlib/amplifier.h"
#include "sprintflst.h"

/* Every meter of the dummy amplifier, as one read of them all */
struct amp_reading
{
    int fwd, peak, reflected, input;
    float swr, swr_tuner, vd, id, temp;
    const char *fault, *warning;
};

/* The meters of a 1 kW amplifier transmitting at full power: what forward
 * power is converted from the supply, the drive its gain needs, the current
 * drawn switched on but idle, and the supply at rest and per ampere drawn */
#define EFFICIENCY 0.63f
#define GAIN 40.0f
#define IDLE_CURRENT 0.5f
#define VD_REST 50.4f
#define VD_SAG_PER_A 0.05f

#define SAMPLE_US 105000

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

static int amp_level_i(AMP *amp, setting_t level)
{
    value_t value = { .i = -1 };
    int status = amp_get_level(amp, level, &value);

    check(status == RIG_OK, "reading %s: %s", amp_strlevel(level),
          rigerror2(status));
    return value.i;
}

static float amp_level_f(AMP *amp, setting_t level)
{
    value_t value = { .f = -1 };
    int status = amp_get_level(amp, level, &value);

    check(status == RIG_OK, "reading %s: %s", amp_strlevel(level),
          rigerror2(status));
    return value.f;
}

static const char *amp_level_s(AMP *amp, setting_t level)
{
    value_t value = { .s = NULL };
    int status = amp_get_level(amp, level, &value);

    check(status == RIG_OK, "reading %s: %s", amp_strlevel(level),
          rigerror2(status));
    return value.s;
}

static void read_amp(AMP *amp, struct amp_reading *r)
{
    r->fwd = amp_level_i(amp, AMP_LEVEL_PWR_FWD);
    r->peak = amp_level_i(amp, AMP_LEVEL_PWR_PEAK);
    r->swr = amp_level_f(amp, AMP_LEVEL_SWR);
    r->reflected = amp_level_i(amp, AMP_LEVEL_PWR_REFLECTED);
    r->swr_tuner = amp_level_f(amp, AMP_LEVEL_SWR_TUNER);
    r->input = amp_level_i(amp, AMP_LEVEL_PWR_INPUT);
    r->vd = amp_level_f(amp, AMP_LEVEL_VD_METER);
    r->id = amp_level_f(amp, AMP_LEVEL_ID_METER);
    r->temp = amp_level_f(amp, AMP_LEVEL_TEMP_METER);
    r->fault = amp_level_s(amp, AMP_LEVEL_FAULT);
    r->warning = amp_level_s(amp, AMP_LEVEL_WARNING);
}

/* Reads every meter within one sample: the reading is taken again when the
 * sample changed while it was read, which the meters read first and again last
 * tell */
static void read_one_sample(AMP *amp, struct amp_reading *r)
{
    int attempt;

    for (attempt = 0; attempt < 10; attempt++)
    {
        int fwd = amp_level_i(amp, AMP_LEVEL_PWR_FWD);
        float swr = amp_level_f(amp, AMP_LEVEL_SWR);
        float vd = amp_level_f(amp, AMP_LEVEL_VD_METER);

        read_amp(amp, r);

        if (fwd == r->fwd && swr == r->swr && vd == r->vd
                && amp_level_i(amp, AMP_LEVEL_PWR_FWD) == fwd
                && amp_level_f(amp, AMP_LEVEL_SWR) == swr
                && amp_level_f(amp, AMP_LEVEL_VD_METER) == vd)
        {
            return;
        }
    }

    check(0, "no reading within one sample in ten attempts");
}

static void set_power(AMP *amp, powerstat_t state)
{
    int status = amp_set_powerstat(amp, state);

    check(status == RIG_OK, "setting power state %d: %s", state,
          rigerror2(status));
}

static void set_pwr(AMP *amp, float level)
{
    value_t value = { .f = level };
    int status = amp_set_level(amp, AMP_LEVEL_PWR, value);

    check(status == RIG_OK, "setting PWR %.2f: %s", level, rigerror2(status));
}

static int transmitting(AMP *amp)
{
    amp_status_t status = 0;

    check(amp_get_status(amp, &status) == RIG_OK, "reading the status");
    return (status & AMP_STATUS_PTT) != 0;
}

static float reflection(float swr)
{
    float gamma = (swr - 1.0f) / (swr + 1.0f);

    return gamma * gamma;
}

static AMP *open_amp(const char *static_data)
{
    AMP *amp = amp_init(AMP_MODEL_DUMMY);

    if (amp == NULL)
    {
        fprintf(stderr, "failed to initialize the dummy amplifier\n");
        exit(1);
    }

    if (static_data != NULL)
    {
        int status = amp_set_conf(amp, amp_token_lookup(amp, "static_data"),
                                  static_data);

        check(status == RIG_OK, "setting static_data: %s", rigerror2(status));
    }

    if (amp_open(amp) != RIG_OK)
    {
        fprintf(stderr, "failed to open the dummy amplifier\n");
        exit(1);
    }

    return amp;
}

static void close_amp(AMP *amp)
{
    amp_close(amp);
    amp_cleanup(amp);
}

/* FAULT and WARNING, TEMP_METER and PWRINPUT are listed among the levels the
 * capabilities' dump prints, and PWR has the range of a float level */
static void test_amp_caps(AMP *amp)
{
    char levels[2048];

    amp_sprintf_level_gran(levels, sizeof(levels), amp->caps->has_get_level,
                           amp->caps->level_gran);

    check(strstr(levels, "FAULT()") != NULL, "FAULT not listed: %s", levels);
    check(strstr(levels, "WARNING()") != NULL, "WARNING not listed: %s",
          levels);
    check(strstr(levels, "TEMP_METER(") != NULL, "TEMP_METER not listed: %s",
          levels);
    check(strstr(levels, "PWRINPUT(") != NULL, "PWRINPUT not listed: %s",
          levels);
    check(strstr(levels, "PWR(0.000000..1.000000/0.010000)") != NULL,
          "PWR not listed with the range 0..1 in steps of 0.01: %s", levels);
}

static void test_amp_off(AMP *amp)
{
    struct amp_reading r;

    check(amp_level_f(amp, AMP_LEVEL_PWR) == 1.0f,
          "the power level does not start at 100 %%");

    read_amp(amp, &r);
    check(r.fwd == 0 && r.peak == 0 && r.reflected == 0 && r.input == 0,
          "off, power reads FWD %d PEAK %d REF %d IN %d", r.fwd, r.peak,
          r.reflected, r.input);
    check(r.swr == 0 && r.swr_tuner == 0 && r.vd == 0 && r.id == 0
          && r.temp == 0, "off, SWR %f TSWR %f VD %f ID %f TEMP %f read",
          r.swr, r.swr_tuner, r.vd, r.id, r.temp);
    check(r.fault != NULL && r.fault[0] == '\0', "off, FAULT reads %s",
          r.fault ? r.fault : "NULL");
    check(r.warning != NULL && r.warning[0] == '\0', "off, WARNING reads %s",
          r.warning ? r.warning : "NULL");
    check(!transmitting(amp), "off, the status is PTT");
}

static void check_idle(AMP *amp, const char *state)
{
    struct amp_reading r;
    int i;

    for (i = 0; i < 5; i++)
    {
        read_one_sample(amp, &r);
        check(r.fwd == 0 && r.peak == 0 && r.reflected == 0 && r.input == 0,
              "%s, power reads FWD %d PEAK %d REF %d IN %d", state, r.fwd,
              r.peak, r.reflected, r.input);
        check(r.swr == 1.0f && r.swr_tuner == 1.0f,
              "%s, SWR %f and TSWR %f, not 1.0", state, r.swr, r.swr_tuner);
        check(r.vd >= 50.3f && r.vd <= 50.45f, "%s, VD %f not at rest",
              state, r.vd);
        check(r.id >= 0.44f && r.id <= 0.56f, "%s, ID %f not idle", state,
              r.id);
        check(r.temp >= 24.5f && r.temp <= 46.0f, "%s, TEMP %f", state,
              r.temp);
        check(r.fault != NULL && r.fault[0] == '\0'
              && r.warning != NULL && r.warning[0] == '\0',
              "%s, a fault or warning reads", state);
        check(!transmitting(amp), "%s, the status is PTT", state);
        usleep(SAMPLE_US);
    }
}

/* Reads samples while transmitting, checking each, and returns their mean
 * forward power */
static float check_transmitting(AMP *amp, float level, int samples,
                                int *distinct_fwd, int *distinct_swr,
                                float *first_temp, float *last_temp)
{
    struct amp_reading r, previous = { 0 };
    float sum = 0, nominal = 1000.0f * level;
    int i;

    *distinct_fwd = *distinct_swr = 0;

    for (i = 0; i < samples; i++)
    {
        float expected_current;

        read_one_sample(amp, &r);

        check(r.fwd >= 0.94f * nominal && r.fwd <= 1.01f * nominal,
              "FWD %d off %.0f W", r.fwd, nominal);
        check(r.peak > r.fwd && r.peak <= 1.07f * r.fwd + 1,
              "PEAK %d not a few percent above FWD %d", r.peak, r.fwd);
        check(r.swr >= 1.19f && r.swr <= 1.31f, "SWR %f", r.swr);
        check(r.swr_tuner == r.swr, "TSWR %f is not SWR %f with the tuner off",
              r.swr_tuner, r.swr);
        check(fabsf(r.reflected - r.fwd * reflection(r.swr)) <= 1.0f,
              "REF %d does not agree with FWD %d and SWR %f", r.reflected,
              r.fwd, r.swr);
        check(fabsf(r.input - r.fwd / GAIN) <= 1.0f,
              "IN %d does not agree with FWD %d", r.input, r.fwd);
        check(r.vd >= 47.0f && r.vd <= 50.0f,
              "VD %f", r.vd);
        expected_current = IDLE_CURRENT + r.fwd / EFFICIENCY / r.vd;
        check(fabsf(r.id - expected_current) <= 0.05f,
              "ID %f does not agree with FWD %d and VD %f (%f)", r.id, r.fwd,
              r.vd, expected_current);
        check(r.temp >= 24.5f && r.temp <= 46.0f, "TEMP %f", r.temp);
        check(r.fault != NULL && r.fault[0] == '\0'
              && r.warning != NULL && r.warning[0] == '\0',
              "transmitting, a fault or warning reads");

        if (i > 0)
        {
            *distinct_fwd += r.fwd != previous.fwd;
            *distinct_swr += r.swr != previous.swr;
            check(fabsf(r.swr - previous.swr) < 0.06f,
                  "SWR stepped from %f to %f", previous.swr, r.swr);
        }
        else
        {
            *first_temp = r.temp;
        }

        *last_temp = r.temp;
        previous = r;
        sum += r.fwd;
        usleep(SAMPLE_US);
    }

    return sum / samples;
}

static void test_amp_meters(void)
{
    AMP *amp = open_amp(NULL);
    float full, half, first_temp, last_temp, cooled;
    int distinct_fwd, distinct_swr;
    struct amp_reading r;
    int i;

    test_amp_caps(amp);
    test_amp_off(amp);

    set_power(amp, RIG_POWER_ON);
    check_idle(amp, "on");
    set_power(amp, RIG_POWER_STANDBY);
    check_idle(amp, "standby");

    set_power(amp, RIG_POWER_OPERATE);
    check(transmitting(amp), "operating, the status is not PTT");
    full = check_transmitting(amp, 1.0f, 25, &distinct_fwd, &distinct_swr,
                              &first_temp, &last_temp);
    check(distinct_fwd >= 5 && distinct_swr >= 5,
          "the meters do not move: FWD changed %d times, SWR %d times in 25 "
          "samples", distinct_fwd, distinct_swr);
    check(last_temp > first_temp, "transmitting, TEMP went from %f to %f",
          first_temp, last_temp);

    set_pwr(amp, 0.5f);
    half = check_transmitting(amp, 0.5f, 10, &distinct_fwd, &distinct_swr,
                              &first_temp, &last_temp);
    check(half / full >= 0.47f && half / full <= 0.53f,
          "at PWR 0.5 the mean FWD is %.0f W, against %.0f W at 1.0", half,
          full);
    set_pwr(amp, 1.0f);

    check(amp_set_func(amp, AMP_FUNC_TUNER, 1) == RIG_OK, "the tuner on");

    for (i = 0; i < 5; i++)
    {
        read_one_sample(amp, &r);
        check(r.swr_tuner >= 1.04f && r.swr_tuner <= 1.08f,
              "with the tuner on, TSWR %f", r.swr_tuner);
        check(r.swr >= 1.19f && r.swr <= 1.31f, "with the tuner on, SWR %f",
              r.swr);
        usleep(SAMPLE_US);
    }

    check(amp_set_func(amp, AMP_FUNC_TUNER, 0) == RIG_OK, "the tuner off");

    set_power(amp, RIG_POWER_STANDBY);
    read_one_sample(amp, &r);
    last_temp = r.temp;
    usleep(15 * SAMPLE_US);
    read_one_sample(amp, &r);
    cooled = r.temp;
    check(cooled < last_temp, "in standby, TEMP went from %f to %f",
          last_temp, cooled);

    set_power(amp, RIG_POWER_OFF);
    test_amp_off(amp);
    close_amp(amp);
}

static void check_fixed(AMP *amp, int fwd, int peak, int reflected,
                        int input, float vd, float id, float temp)
{
    struct amp_reading r;
    int i;

    for (i = 0; i < 3; i++)
    {
        read_amp(amp, &r);
        check(r.fwd == fwd && r.peak == peak && r.reflected == reflected
              && r.input == input,
              "static, FWD %d PEAK %d REF %d IN %d, not %d %d %d %d", r.fwd,
              r.peak, r.reflected, r.input, fwd, peak, reflected, input);
        check(fabsf(r.swr - (fwd > 0 ? 1.25f : 1.0f)) < 1e-4f
              && r.swr_tuner == r.swr, "static, SWR %f TSWR %f", r.swr,
              r.swr_tuner);
        check(fabsf(r.vd - vd) < 0.01f && fabsf(r.id - id) < 0.01f,
              "static, VD %f ID %f, not %f %f", r.vd, r.id, vd, id);
        check(fabsf(r.temp - temp) < 0.05f, "static, TEMP %f, not %f", r.temp,
              temp);
        usleep(SAMPLE_US);
    }
}

static void test_amp_static_data(void)
{
    AMP *amp = open_amp("1");

    set_power(amp, RIG_POWER_OPERATE);
    check_fixed(amp, 1000, 1040, 12, 25, 48.80f, 33.03f, 45.0f);
    set_pwr(amp, 0.5f);
    check_fixed(amp, 500, 520, 6, 13, 49.59f, 16.50f, 35.4f);
    set_power(amp, RIG_POWER_STANDBY);
    check_fixed(amp, 0, 0, 0, 0, 50.375f, 0.5f, 25.0f);
    close_amp(amp);
}

static float rig_level_f(RIG *rig, setting_t level)
{
    value_t value = { .f = -1 };
    int status = rig_get_level(rig, RIG_VFO_CURR, level, &value);

    check(status == RIG_OK, "reading the rig's %s: %s", rig_strlevel(level),
          rigerror2(status));
    return value.f;
}

static RIG *open_rig(const char *static_data)
{
    RIG *rig = rig_init(RIG_MODEL_DUMMY);

    if (rig == NULL)
    {
        fprintf(stderr, "failed to initialize the dummy rig\n");
        exit(1);
    }

    if (static_data != NULL)
    {
        int status = rig_set_conf(rig, rig_token_lookup(rig, "static_data"),
                                  static_data);

        check(status == RIG_OK, "setting the rig's static_data: %s",
              rigerror2(status));
    }

    if (rig_open(rig) != RIG_OK)
    {
        fprintf(stderr, "failed to open the dummy rig\n");
        exit(1);
    }

    return rig;
}

static void test_rig_meters(void)
{
    RIG *rig = open_rig(NULL);
    float swr, alc, temp, last_swr = 0, last_alc = 0, last_temp = 0;
    int moved_swr = 0, moved_alc = 0, moved_temp = 0;
    int i;

    for (i = 0; i < 20; i++)
    {
        swr = rig_level_f(rig, RIG_LEVEL_SWR);
        alc = rig_level_f(rig, RIG_LEVEL_ALC);
        temp = rig_level_f(rig, RIG_LEVEL_TEMP_METER);
        check(swr >= 1.2f && swr <= 1.5f, "the rig's SWR %f", swr);
        check(alc >= 0.2f && alc <= 0.7f, "the rig's ALC %f", alc);
        check(temp >= 33.0f && temp <= 43.5f, "the rig's TEMP_METER %f", temp);

        if (i > 0)
        {
            moved_swr += swr != last_swr;
            moved_alc += alc != last_alc;
            moved_temp += temp != last_temp;
        }

        last_swr = swr;
        last_alc = alc;
        last_temp = temp;
        usleep(20000);
    }

    check(moved_swr > 0 && moved_alc > 0 && moved_temp > 0,
          "the rig's meters do not move: SWR %d, ALC %d, TEMP_METER %d",
          moved_swr, moved_alc, moved_temp);
    rig_close(rig);
    rig_cleanup(rig);

    rig = open_rig("1");
    check(rig_level_f(rig, RIG_LEVEL_SWR) == 1.3f, "the rig's static SWR");
    check(rig_level_f(rig, RIG_LEVEL_ALC) == 0.4f, "the rig's static ALC");
    check(rig_level_f(rig, RIG_LEVEL_TEMP_METER) == 38.0f,
          "the rig's static TEMP_METER");
    rig_close(rig);
    rig_cleanup(rig);
}

int main(void)
{
    rig_set_debug(RIG_DEBUG_ERR);

    test_amp_meters();
    test_amp_static_data();
    test_rig_meters();

    return failed;
}
