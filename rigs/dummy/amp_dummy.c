/*
 *  Hamlib Dummy backend - main file
 *  Copyright (c) 2001-2009 by Stephane Fillod
 *
 *
 *   This library is free software; you can redistribute it and/or
 *   modify it under the terms of the GNU Lesser General Public
 *   License as published by the Free Software Foundation; either
 *   version 2.1 of the License, or (at your option) any later version.
 *
 *   This library is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *   Lesser General Public License for more details.
 *
 *   You should have received a copy of the GNU Lesser General Public
 *   License along with this library; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/time.h>

#include "hamlib/amplifier.h"
#include "hamlib/port.h"
#include "hamlib/amp_state.h"
#include "register.h"

#include "idx_builtin.h"
#include "bandplan.h"
#include "dummy_common.h"
#include "amp_dummy.h"

#define DUMMY_AMP_INPUTS (RIG_ANT_1 | RIG_ANT_2)
#define DUMMY_AMP_ANTS (RIG_ANT_1 | RIG_ANT_2 | RIG_ANT_3 | RIG_ANT_4)

#define DUMMY_AMP_AMP_OPS (AMP_OP_TUNE | AMP_OP_BAND_UP | AMP_OP_BAND_DOWN | AMP_OP_L_NH_UP | AMP_OP_L_NH_DOWN | AMP_OP_C_PF_UP | AMP_OP_C_PF_DOWN)

#define DUMMY_AMP_GET_FUNCS (AMP_FUNC_TUNER)
#define DUMMY_AMP_SET_FUNCS (AMP_FUNC_TUNER)
#define DUMMY_AMP_GET_LEVELS (AMP_LEVEL_SWR | AMP_LEVEL_SWR_TUNER | AMP_LEVEL_PWR | AMP_LEVEL_PWR_INPUT | AMP_LEVEL_PWR_FWD | AMP_LEVEL_PWR_REFLECTED | AMP_LEVEL_PWR_PEAK | AMP_LEVEL_FAULT | AMP_LEVEL_WARNING | AMP_LEVEL_VD_METER | AMP_LEVEL_ID_METER | AMP_LEVEL_TEMP_METER)
#define DUMMY_AMP_SET_LEVELS (AMP_LEVEL_PWR)
#define DUMMY_AMP_GET_PARMS (AMP_PARM_BACKLIGHT)
#define DUMMY_AMP_SET_PARMS (AMP_PARM_BACKLIGHT)

/*
 * The dummy amplifier is a 1 kW solid-state amplifier on a 50 V supply. Off,
 * its meters read zero; on or in standby it is idle; in operate it transmits,
 * its forward power the rated power times its power level.
 */
#define DUMMY_AMP_RATED_W        1000.0f /* forward power at a power level of 1.0 */
#define DUMMY_AMP_GAIN           40.0f   /* 16 dB: 25 W of drive for 1 kW */
#define DUMMY_AMP_PEAK           1.04f   /* peak over forward power */
#define DUMMY_AMP_SWR            1.25f   /* the antenna's */
#define DUMMY_AMP_SWR_TUNED      1.06f   /* what the tuner, when on, presents to the amplifier */
#define DUMMY_AMP_EFFICIENCY     0.63f   /* forward power over the supply's power */
#define DUMMY_AMP_VD_REST        50.4f   /* the supply, nothing drawn */
#define DUMMY_AMP_VD_SAG         0.05f   /* the supply's drop per ampere drawn */
#define DUMMY_AMP_ID_IDLE        0.5f    /* the current drawn when switched on */
#define DUMMY_AMP_TEMP_AMBIENT   25.0f
#define DUMMY_AMP_TEMP_PER_W     0.0327f /* heatsink rise per watt dissipated: 45 degrees C at 1 kW */
#define DUMMY_AMP_TEMP_TIME_S    120.0f  /* the heatsink's time constant */
#define DUMMY_AMP_SAMPLE_MS      100     /* how long one sample of the meters stands */

/* Every meter at one moment, read together so that one poll's readings agree */
struct dummy_amp_meters
{
    int fwd;
    int peak;
    int reflected;
    int input;
    float swr;
    float swr_tuner;
    float vd;
    float id;
    float temp;
};

struct dummy_amp_priv_data
{
    freq_t freq;
    powerstat_t powerstat;
    ant_t input;
    ant_t antenna;

    amp_status_t status;

    setting_t funcs;
    value_t levels[RIG_SETTING_MAX];
    value_t parms[RIG_SETTING_MAX];

    struct ext_list *ext_funcs;
    struct ext_list *ext_levels;
    struct ext_list *ext_parms;

    char *magic_conf;
    int static_data;

    struct dummy_amp_meters meters;
    struct timeval sampled;  /* when the meters were sampled; zero for none */
    float heatsink;          /* the heatsink's temperature, whatever the power state */
    struct timeval heated;   /* when the heatsink was last brought up to date */
};

static const struct confparams dummy_ext_levels[] =
{
    {
        TOK_EL_AMP_MAGICLEVEL, "MGL", "Magic level", "Magic level, as an example",
        NULL, RIG_CONF_NUMERIC, { .n = { 0, 1, .001f } }
    },
    {
        TOK_EL_AMP_MAGICFUNC, "MGF", "Magic func", "Magic function, as an example",
        NULL, RIG_CONF_CHECKBUTTON
    },
    {
        TOK_EL_AMP_MAGICOP, "MGO", "Magic Op", "Magic Op, as an example",
        NULL, RIG_CONF_BUTTON
    },
    {
        TOK_EL_AMP_MAGICCOMBO, "MGC", "Magic combo", "Magic combo, as an example",
        "VALUE1", RIG_CONF_COMBO, { .c = { .combostr = { "VALUE1", "VALUE2", "NONE", NULL } } }
    },
    { RIG_CONF_END, NULL, }
};

static const struct confparams dummy_ext_funcs[] =
{
    {
        TOK_EL_AMP_MAGICEXTFUNC, "MGEF", "Magic ext func", "Magic ext function, as an example",
        NULL, RIG_CONF_CHECKBUTTON
    },
    { RIG_CONF_END, NULL, }
};

static const struct confparams dummy_ext_parms[] =
{
    {
        TOK_EP_AMP_MAGICPARM, "MGP", "Magic parm", "Magic parameter, as an example",
        NULL, RIG_CONF_NUMERIC, { .n = { 0, 1, .001f } }
    },
    { RIG_CONF_END, NULL, }
};

/* cfgparams are configuration item generally used by the backend's open() method */
static const struct confparams dummy_cfg_params[] =
{
    {
        TOK_CFG_AMP_MAGICCONF, "mcfg", "Magic conf", "Magic parameter, as an example",
        "AMPLIFIER", RIG_CONF_STRING, { }
    },
    {
        TOK_CFG_AMP_STATIC_DATA, "static_data", "Static data", "Output only static data, no randomization of meter values",
        "0", RIG_CONF_CHECKBUTTON, { }
    },
    { RIG_CONF_END, NULL, }
};

/* Milliseconds from one time to a later one */
static long dummy_amp_elapsed_ms(const struct timeval *from, const struct timeval *to)
{
    return (to->tv_sec - from->tv_sec) * 1000L + (to->tv_usec - from->tv_usec) / 1000L;
}

/* A random number from -1.0 to 1.0, in steps of a tenth, for the meters' noise */
static float dummy_amp_noise(void)
{
    return (float)(rand() % 21 - 10) / 10.0f;
}

/* The supply's voltage and current for a forward power, none when idle, with
 * the drop the current makes */
static void dummy_amp_supply(float fwd, float noise_v, float *vd, float *id)
{
    float supplied = fwd / DUMMY_AMP_EFFICIENCY;

    *vd = DUMMY_AMP_VD_REST - DUMMY_AMP_VD_SAG * (DUMMY_AMP_ID_IDLE + supplied / DUMMY_AMP_VD_REST)
          + noise_v;
    *id = DUMMY_AMP_ID_IDLE + supplied / *vd;
}

/* The temperature the heatsink settles at in the power state, at the power
 * level: what transmitting dissipates warms it, and the fans keep it at the
 * ambient temperature otherwise */
static float dummy_amp_settled_temp(powerstat_t powerstat, float level)
{
    float fwd = DUMMY_AMP_RATED_W * level, vd, id;

    if (powerstat != RIG_POWER_OPERATE)
    {
        return DUMMY_AMP_TEMP_AMBIENT;
    }

    dummy_amp_supply(fwd, 0, &vd, &id);
    return DUMMY_AMP_TEMP_AMBIENT + DUMMY_AMP_TEMP_PER_W * (vd * id - fwd);
}

/* Brings the heatsink up to now, at what the power state it has been in
 * dissipates: it warms while transmitting and cools while not */
static void dummy_amp_heat(struct dummy_amp_priv_data *priv, const struct timeval *now)
{
    float settled = dummy_amp_settled_temp(priv->powerstat, priv->levels[AMP_LVL_PWR].f);
    float elapsed_s = (float)dummy_amp_elapsed_ms(&priv->heated, now) / 1000.0f;

    if (elapsed_s > 0)
    {
        priv->heatsink = settled + (priv->heatsink - settled) * expf(-elapsed_s / DUMMY_AMP_TEMP_TIME_S);
        priv->heated = *now;
    }
}

/* Samples every meter, once the last sample has stood its time, from the power
 * state and the power level: off, all read zero; on or in standby, idle; in
 * operate, transmitting, with a slow drift, as the dummy rig's meters drift,
 * and noise in each sample, or fixed with static_data */
static void dummy_amp_sample(struct dummy_amp_priv_data *priv)
{
    struct dummy_amp_meters *m = &priv->meters;
    struct timeval now;
    float level = priv->levels[AMP_LVL_PWR].f;
    float fwd = 0;

    gettimeofday(&now, NULL);

    if ((priv->sampled.tv_sec != 0 || priv->sampled.tv_usec != 0)
            && dummy_amp_elapsed_ms(&priv->sampled, &now) < DUMMY_AMP_SAMPLE_MS)
    {
        return;
    }

    priv->sampled = now;
    dummy_amp_heat(priv, &now);
    memset(m, 0, sizeof(*m));

    if (priv->powerstat != RIG_POWER_ON && priv->powerstat != RIG_POWER_STANDBY
            && priv->powerstat != RIG_POWER_OPERATE)
    {
        return;
    }

    m->swr = 1.0f;
    m->swr_tuner = 1.0f;

    if (priv->powerstat == RIG_POWER_OPERATE)
    {
        float ripple = 1.0f, peak = DUMMY_AMP_PEAK;
        float swr = DUMMY_AMP_SWR, tuned = DUMMY_AMP_SWR_TUNED;
        float gamma;

        if (!priv->static_data)
        {
            /* Drifting over 16 s for the power and 20 s for the SWR */
            ripple = 0.96f + 0.0025f * (float)(now.tv_sec % 16) + 0.01f * dummy_amp_noise();
            peak = 1.03f + 0.015f * (dummy_amp_noise() + 1.0f);
            swr += 0.04f * (fabsf((float)(now.tv_sec % 20) - 10.0f) - 5.0f) / 5.0f
                   + 0.01f * dummy_amp_noise();
            tuned += 0.01f * dummy_amp_noise();
        }

        fwd = DUMMY_AMP_RATED_W * level * ripple;
        gamma = (swr - 1.0f) / (swr + 1.0f);

        m->fwd = (int)lroundf(fwd);
        m->peak = (int)lroundf(fwd * peak);
        m->reflected = (int)lroundf(fwd * gamma * gamma);
        m->input = (int)lroundf(fwd / DUMMY_AMP_GAIN);
        m->swr = swr;
        m->swr_tuner = (priv->funcs & AMP_FUNC_TUNER) ? tuned : swr;
    }

    dummy_amp_supply(fwd, priv->static_data ? 0 : 0.05f * dummy_amp_noise(), &m->vd, &m->id);

    if (priv->powerstat != RIG_POWER_OPERATE && !priv->static_data)
    {
        m->id += 0.05f * dummy_amp_noise();
    }

    m->temp = priv->static_data ? dummy_amp_settled_temp(priv->powerstat, level) : priv->heatsink;
}

/* Lets the meters follow a change at once: a new sample at the next read, the
 * heatsink brought up to the change first */
static void dummy_amp_changed(struct dummy_amp_priv_data *priv)
{
    struct timeval now;

    gettimeofday(&now, NULL);
    dummy_amp_heat(priv, &now);
    priv->sampled.tv_sec = 0;
    priv->sampled.tv_usec = 0;
}

static int dummy_amp_init(AMP *amp)
{
    struct dummy_amp_priv_data *priv;

    rig_debug(RIG_DEBUG_VERBOSE, "%s called\n", __func__);

    AMPSTATE(amp)->priv = (struct dummy_amp_priv_data *)
                          calloc(1, sizeof(struct dummy_amp_priv_data));

    if (!AMPSTATE(amp)->priv)
    {
        return -RIG_ENOMEM;
    }

    priv = AMPSTATE(amp)->priv;

    priv->ext_funcs = alloc_init_ext(dummy_ext_funcs);

    if (!priv->ext_funcs)
    {
        free(AMPSTATE(amp)->priv);
        AMPSTATE(amp)->priv = NULL;
        return -RIG_ENOMEM;
    }

    priv->ext_levels = alloc_init_ext(dummy_ext_levels);

    if (!priv->ext_levels)
    {
        free(priv->ext_funcs);
        free(AMPSTATE(amp)->priv);
        AMPSTATE(amp)->priv = NULL;
        return -RIG_ENOMEM;
    }

    priv->ext_parms = alloc_init_ext(dummy_ext_parms);

    if (!priv->ext_parms)
    {
        free(priv->ext_levels);
        free(priv->ext_funcs);
        free(AMPSTATE(amp)->priv);
        AMPSTATE(amp)->priv = NULL;
        return -RIG_ENOMEM;
    }

    AMPPORT(amp)->type.rig = RIG_PORT_NONE;

    priv->freq = 0;
    priv->input = RIG_ANT_1;
    priv->antenna = RIG_ANT_1;
    priv->levels[AMP_LVL_PWR].f = 1.0f;
    priv->heatsink = DUMMY_AMP_TEMP_AMBIENT;
    gettimeofday(&priv->heated, NULL);

    priv->magic_conf = strdup("AMPLIFIER");

    return RIG_OK;
}

static int dummy_amp_cleanup(AMP *amp)
{
    const struct dummy_amp_priv_data *priv = (struct dummy_amp_priv_data *)
            AMPSTATE(amp)->priv;

    rig_debug(RIG_DEBUG_VERBOSE, "%s called\n", __func__);

    if (AMPSTATE(amp)->priv)
    {
        free(priv->ext_funcs);
        free(priv->ext_levels);
        free(priv->ext_parms);
        free(priv->magic_conf);
        free(AMPSTATE(amp)->priv);
    }

    AMPSTATE(amp)->priv = NULL;

    return RIG_OK;
}

static int dummy_amp_open(AMP *amp)
{
    rig_debug(RIG_DEBUG_VERBOSE, "%s called\n", __func__);

    return RIG_OK;
}

static int dummy_amp_close(AMP *amp)
{
    rig_debug(RIG_DEBUG_VERBOSE, "%s called\n", __func__);

    return RIG_OK;
}

static int dummy_set_conf(AMP *amp, hamlib_token_t token, const char *val)
{
    struct dummy_amp_priv_data *priv;

    priv = (struct dummy_amp_priv_data *) AMPSTATE(amp)->priv;

    switch (token)
    {
    case TOK_CFG_AMP_MAGICCONF:
        if (val)
        {
            free(priv->magic_conf);
            priv->magic_conf = strdup(val);
        }

        break;

    case TOK_CFG_AMP_STATIC_DATA:
        priv->static_data = atoi(val) ? 1 : 0;
        dummy_amp_changed(priv);
        break;

    default:
        return -RIG_EINVAL;
    }

    return RIG_OK;
}


static int dummy_get_conf2(AMP *amp, hamlib_token_t token, char *val, int val_len)
{
    struct dummy_amp_priv_data *priv;

    priv = (struct dummy_amp_priv_data *) AMPSTATE(amp)->priv;

    switch (token)
    {
    case TOK_CFG_AMP_MAGICCONF:
        SNPRINTF(val, val_len, "%s", priv->magic_conf);
        break;

    case TOK_CFG_AMP_STATIC_DATA:
        SNPRINTF(val, val_len, "%d", priv->static_data);
        break;

    default:
        return -RIG_EINVAL;
    }

    return RIG_OK;
}

static int dummy_get_conf(AMP *amp, hamlib_token_t token, char *val)
{
    return dummy_get_conf2(amp, token, val, 128);
}


static int dummy_amp_reset(AMP *amp, amp_reset_t reset)
{
    rig_debug(RIG_DEBUG_VERBOSE, "%s called\n", __func__);

    switch (reset)
    {
    case AMP_RESET_MEM:
        rig_debug(RIG_DEBUG_VERBOSE, "%s: Reset memory\n", __func__);
        break;

    case AMP_RESET_FAULT:
        rig_debug(RIG_DEBUG_VERBOSE, "%s: Reset fault\n", __func__);
        break;

    case AMP_RESET_AMP:
        rig_debug(RIG_DEBUG_VERBOSE, "%s: Reset amplifier\n", __func__);
        break;

    default:
        rig_debug(RIG_DEBUG_VERBOSE, "%s: Reset unknown=%d\n", __func__, reset);
        return -RIG_EINVAL;
    }

    return RIG_OK;
}

/*
^ON1 turns amp on
^ON0 turns amp off
^OP0 amp in standby
^OP1 amp in operate

^PWF which gets forward power from amp. Reply would be ^PWFnnnn format

^PWK which gets peak forward power. Response is ^PWKnnnn

^PWR peak reflected power. Response is ^PWRnnnn

^SW gets SWR. Response is ^SWnnn. Example return of ^SW025 would be 2.5:1

Also a way to display faults (there are commands)
*/

static int dummy_amp_get_freq(AMP *amp, freq_t *freq)
{
    const struct dummy_amp_priv_data *priv = (struct dummy_amp_priv_data *)
            AMPSTATE(amp)->priv;
    rig_debug(RIG_DEBUG_VERBOSE, "%s called\n", __func__);
    *freq = priv->freq;
    return RIG_OK;
}

static int dummy_amp_set_freq(AMP *amp, freq_t freq)
{
    struct dummy_amp_priv_data *priv = (struct dummy_amp_priv_data *)
                                       AMPSTATE(amp)->priv;
    rig_debug(RIG_DEBUG_VERBOSE, "%s called\n", __func__);
    priv->freq = freq;
    return RIG_OK;
}

static int dummy_amp_get_input(AMP *amp, ant_t *input)
{
    const struct dummy_amp_priv_data *priv = (struct dummy_amp_priv_data *)
            AMPSTATE(amp)->priv;
    rig_debug(RIG_DEBUG_VERBOSE, "%s called\n", __func__);
    *input = priv->input;
    return RIG_OK;
}

static int dummy_amp_set_input(AMP *amp, ant_t input)
{
    struct dummy_amp_priv_data *priv = (struct dummy_amp_priv_data *)
                                       AMPSTATE(amp)->priv;
    rig_debug(RIG_DEBUG_VERBOSE, "%s called\n", __func__);
    priv->input = input;
    return RIG_OK;
}

static int dummy_amp_get_ant(AMP *amp, ant_t *ant)
{
    const struct dummy_amp_priv_data *priv = (struct dummy_amp_priv_data *)
            AMPSTATE(amp)->priv;
    rig_debug(RIG_DEBUG_VERBOSE, "%s called\n", __func__);
    *ant = priv->antenna;
    return RIG_OK;
}

static int dummy_amp_set_ant(AMP *amp, ant_t ant)
{
    struct dummy_amp_priv_data *priv = (struct dummy_amp_priv_data *)
                                       AMPSTATE(amp)->priv;
    rig_debug(RIG_DEBUG_VERBOSE, "%s called\n", __func__);
    priv->antenna = ant;
    return RIG_OK;
}

static const char *dummy_amp_get_info(AMP *amp)
{
    rig_debug(RIG_DEBUG_VERBOSE, "%s called\n", __func__);

    return "Dummy amplifier";
}

static int dummy_set_level(AMP *amp, setting_t level, value_t val)
{
    struct dummy_amp_priv_data *priv = (struct dummy_amp_priv_data *)
            AMPSTATE(amp)->priv;
    int idx;
    char lstr[32];

    idx = rig_setting2idx(level);

    if (idx >= RIG_SETTING_MAX)
    {
        return -RIG_EINVAL;
    }

    if (level == AMP_LEVEL_PWR)
    {
        dummy_amp_changed(priv);
    }

    priv->levels[idx] = val;

    if (AMP_LEVEL_IS_FLOAT(level))
    {
        SNPRINTF(lstr, sizeof(lstr), "%f", val.f);
    }
    else
    {
        SNPRINTF(lstr, sizeof(lstr), "%d", val.i);
    }

    rig_debug(RIG_DEBUG_VERBOSE, "%s called: %s %s\n", __func__,
              amp_strlevel(level), lstr);

    return RIG_OK;
}

static int dummy_get_level(AMP *amp, setting_t level, value_t *val)
{
    struct dummy_amp_priv_data *priv = (struct dummy_amp_priv_data *)
                                       AMPSTATE(amp)->priv;
    const struct dummy_amp_meters *m = &priv->meters;
    int idx;

    rig_debug(RIG_DEBUG_VERBOSE, "%s called: %s\n", __func__,
              amp_strlevel(level));

    if (!(amp->caps->has_get_level & level))
    {
        return -RIG_EINVAL;
    }

    dummy_amp_sample(priv);

    switch (level)
    {
    case AMP_LEVEL_SWR:
        val->f = m->swr;
        return RIG_OK;

    case AMP_LEVEL_SWR_TUNER:
        val->f = m->swr_tuner;
        return RIG_OK;

    case AMP_LEVEL_PWR_INPUT:
        val->i = m->input;
        return RIG_OK;

    case AMP_LEVEL_PWR_FWD:
        val->i = m->fwd;
        return RIG_OK;

    case AMP_LEVEL_PWR_REFLECTED:
        val->i = m->reflected;
        return RIG_OK;

    case AMP_LEVEL_PWR_PEAK:
        val->i = m->peak;
        return RIG_OK;

    case AMP_LEVEL_VD_METER:
        val->f = m->vd;
        return RIG_OK;

    case AMP_LEVEL_ID_METER:
        val->f = m->id;
        return RIG_OK;

    case AMP_LEVEL_TEMP_METER:
        val->f = m->temp;
        return RIG_OK;

    /* The dummy amplifier has no faults and no warnings */
    case AMP_LEVEL_FAULT:
    case AMP_LEVEL_WARNING:
        val->s = "";
        return RIG_OK;

    default:
        break;
    }

    idx = rig_setting2idx(level);

    if (idx >= RIG_SETTING_MAX)
    {
        return -RIG_EINVAL;
    }

    *val = priv->levels[idx];

    return RIG_OK;
}

static int dummy_set_ext_level(AMP *amp, hamlib_token_t token, value_t val)
{
    const struct dummy_amp_priv_data *priv = (struct dummy_amp_priv_data *)
            AMPSTATE(amp)->priv;
    char lstr[64];
    const struct confparams *cfp;
    struct ext_list *elp;

    cfp = amp_ext_lookup_tok(amp, token);

    if (!cfp)
    {
        return -RIG_EINVAL;
    }

    switch (token)
    {
    case TOK_EL_AMP_MAGICLEVEL:
    case TOK_EL_AMP_MAGICFUNC:
    case TOK_EL_AMP_MAGICOP:
    case TOK_EL_AMP_MAGICCOMBO:
        break;

    default:
        return -RIG_EINVAL;
    }

    switch (cfp->type)
    {
    case RIG_CONF_STRING:
        SNPRINTF(lstr, sizeof(lstr), "%s", val.s);
        break;

    case RIG_CONF_COMBO:
        SNPRINTF(lstr, sizeof(lstr), "%d", val.i);
        break;

    case RIG_CONF_NUMERIC:
        SNPRINTF(lstr, sizeof(lstr), "%f", val.f);
        break;

    case RIG_CONF_CHECKBUTTON:
        SNPRINTF(lstr, sizeof(lstr), "%s", val.i ? "ON" : "OFF");
        break;

    case RIG_CONF_BUTTON:
        lstr[0] = '\0';
        break;

    default:
        return -RIG_EINTERNAL;
    }

    elp = find_ext(priv->ext_levels, token);

    if (!elp)
    {
        return -RIG_EINTERNAL;
    }

    /* store value */
    elp->val = val;

    rig_debug(RIG_DEBUG_VERBOSE, "%s called: %s %s\n", __func__,
              cfp->name, lstr);

    return RIG_OK;
}

static int dummy_get_ext_level(AMP *amp, hamlib_token_t token, value_t *val)
{
    const struct dummy_amp_priv_data *priv = (struct dummy_amp_priv_data *)
            AMPSTATE(amp)->priv;
    const struct confparams *cfp;
    struct ext_list *elp;

    cfp = amp_ext_lookup_tok(amp, token);

    if (!cfp)
    {
        return -RIG_EINVAL;
    }

    switch (token)
    {
    case TOK_EL_AMP_MAGICLEVEL:
    case TOK_EL_AMP_MAGICFUNC:
    case TOK_EL_AMP_MAGICOP:
    case TOK_EL_AMP_MAGICCOMBO:
        break;

    default:
        return -RIG_EINVAL;
    }

    elp = find_ext(priv->ext_levels, token);

    if (!elp)
    {
        return -RIG_EINTERNAL;
    }

    /* load value */
    *val = elp->val;

    rig_debug(RIG_DEBUG_VERBOSE, "%s called: %s\n", __func__,
              cfp->name);

    return RIG_OK;
}


static int dummy_set_ext_func(AMP *amp, hamlib_token_t token, int status)
{
    const struct dummy_amp_priv_data *priv = (struct dummy_amp_priv_data *)
            AMPSTATE(amp)->priv;
    const struct confparams *cfp;
    struct ext_list *elp;

    cfp = amp_ext_lookup_tok(amp, token);

    if (!cfp)
    {
        return -RIG_EINVAL;
    }

    switch (token)
    {
    case TOK_EL_AMP_MAGICEXTFUNC:
        break;

    default:
        return -RIG_EINVAL;
    }

    switch (cfp->type)
    {
    case RIG_CONF_CHECKBUTTON:
        break;

    case RIG_CONF_BUTTON:
        break;

    default:
        return -RIG_EINTERNAL;
    }

    elp = find_ext(priv->ext_funcs, token);

    if (!elp)
    {
        return -RIG_EINTERNAL;
    }

    /* store value */
    elp->val.i = status;

    rig_debug(RIG_DEBUG_VERBOSE, "%s called: %s %d\n", __func__,
              cfp->name, status);

    return RIG_OK;
}


static int dummy_get_ext_func(AMP *amp, hamlib_token_t token, int *status)
{
    const struct dummy_amp_priv_data *priv = (struct dummy_amp_priv_data *)
            AMPSTATE(amp)->priv;
    const struct confparams *cfp;
    struct ext_list *elp;

    cfp = amp_ext_lookup_tok(amp, token);

    if (!cfp)
    {
        return -RIG_EINVAL;
    }

    switch (token)
    {
    case TOK_EL_AMP_MAGICEXTFUNC:
        break;

    default:
        return -RIG_EINVAL;
    }

    elp = find_ext(priv->ext_funcs, token);

    if (!elp)
    {
        return -RIG_EINTERNAL;
    }

    /* load value */
    *status = elp->val.i;

    rig_debug(RIG_DEBUG_VERBOSE, "%s called: %s\n", __func__,
              cfp->name);

    return RIG_OK;
}


static int dummy_set_parm(AMP *amp, setting_t parm, value_t val)
{
    struct dummy_amp_priv_data *priv = (struct dummy_amp_priv_data *)
            AMPSTATE(amp)->priv;
    int idx;
    char pstr[32];

    idx = rig_setting2idx(parm);

    if (idx >= RIG_SETTING_MAX)
    {
        return -RIG_EINVAL;
    }

    if (AMP_PARM_IS_FLOAT(parm))
    {
        SNPRINTF(pstr, sizeof(pstr), "%f", val.f);
    }
    else
    {
        SNPRINTF(pstr, sizeof(pstr), "%d", val.i);
    }

    rig_debug(RIG_DEBUG_VERBOSE, "%s called: %s %s\n", __func__,
              amp_strparm(parm), pstr);

    priv->parms[idx] = val;

    return RIG_OK;
}


static int dummy_get_parm(AMP *amp, setting_t parm, value_t *val)
{
    const struct dummy_amp_priv_data *priv = (struct dummy_amp_priv_data *)
            AMPSTATE(amp)->priv;
    int idx;

    idx = rig_setting2idx(parm);

    if (idx >= RIG_SETTING_MAX)
    {
        return -RIG_EINVAL;
    }

    *val = priv->parms[idx];

    rig_debug(RIG_DEBUG_VERBOSE, "%s called %s\n", __func__,
              amp_strparm(parm));

    return RIG_OK;
}

static int dummy_set_ext_parm(AMP *amp, hamlib_token_t token, value_t val)
{
    const struct dummy_amp_priv_data *priv = (struct dummy_amp_priv_data *)
            AMPSTATE(amp)->priv;
    char lstr[64];
    const struct confparams *cfp;
    struct ext_list *epp;

    cfp = amp_ext_lookup_tok(amp, token);

    if (!cfp)
    {
        return -RIG_EINVAL;
    }

    switch (token)
    {
    case TOK_EP_AMP_MAGICPARM:
        break;

    default:
        return -RIG_EINVAL;
    }

    switch (cfp->type)
    {
    case RIG_CONF_STRING:
        SNPRINTF(lstr, sizeof(lstr), "%s", val.s);
        break;

    case RIG_CONF_COMBO:
        SNPRINTF(lstr, sizeof(lstr), "%d", val.i);
        break;

    case RIG_CONF_NUMERIC:
        SNPRINTF(lstr, sizeof(lstr), "%f", val.f);
        break;

    case RIG_CONF_CHECKBUTTON:
        SNPRINTF(lstr, sizeof(lstr), "%s", val.i ? "ON" : "OFF");
        break;

    case RIG_CONF_BUTTON:
        lstr[0] = '\0';
        break;

    default:
        return -RIG_EINTERNAL;
    }

    epp = find_ext(priv->ext_parms, token);

    if (!epp)
    {
        return -RIG_EINTERNAL;
    }

    /* store value */
    epp->val = val;

    rig_debug(RIG_DEBUG_VERBOSE, "%s called: %s %s\n", __func__,
              cfp->name, lstr);

    return RIG_OK;
}

static int dummy_get_ext_parm(AMP *amp, hamlib_token_t token, value_t *val)
{
    const struct dummy_amp_priv_data *priv = (struct dummy_amp_priv_data *)
            AMPSTATE(amp)->priv;
    const struct confparams *cfp;
    struct ext_list *epp;

    cfp = amp_ext_lookup_tok(amp, token);

    if (!cfp)
    {
        return -RIG_EINVAL;
    }

    switch (token)
    {
    case TOK_EP_AMP_MAGICPARM:
        break;

    default:
        return -RIG_EINVAL;
    }

    epp = find_ext(priv->ext_parms, token);

    if (!epp)
    {
        return -RIG_EINTERNAL;
    }

    /* load value */
    *val = epp->val;

    rig_debug(RIG_DEBUG_VERBOSE, "%s called: %s\n", __func__,
              cfp->name);

    return RIG_OK;
}


static int dummy_amp_get_status(AMP *amp, amp_status_t *status)
{
    struct dummy_amp_priv_data *priv = (struct dummy_amp_priv_data *)
            AMPSTATE(amp)->priv;

    /* Transmitting in operate, as its meters say */
    if (priv->powerstat == RIG_POWER_OPERATE)
    {
        priv->status |= AMP_STATUS_PTT;
    }
    else
    {
        priv->status &= ~AMP_STATUS_PTT;
    }

    *status = priv->status;

    return RIG_OK;
}


static int dummy_amp_set_powerstat(AMP *amp, powerstat_t status)
{
    struct dummy_amp_priv_data *priv = (struct dummy_amp_priv_data *)
                                       AMPSTATE(amp)->priv;

    switch (status)
    {
    case RIG_POWER_OFF:
        rig_debug(RIG_DEBUG_VERBOSE, "%s called POWER_OFF\n", __func__);
        break;

    case RIG_POWER_ON:
        rig_debug(RIG_DEBUG_VERBOSE, "%s called POWER_ON\n", __func__);
        break;

    case RIG_POWER_STANDBY:
        rig_debug(RIG_DEBUG_VERBOSE, "%s called POWER_STANDBY\n", __func__);
        break;

    case RIG_POWER_OPERATE:
        rig_debug(RIG_DEBUG_VERBOSE, "%s called POWER_OPERATE\n", __func__);
        break;

    case RIG_POWER_UNKNOWN:
        rig_debug(RIG_DEBUG_VERBOSE, "%s called POWER_UNKNOWN\n", __func__);
        break;

    default:
        rig_debug(RIG_DEBUG_VERBOSE, "%s called invalid power status=%d\n",
                  __func__, status);
        return -RIG_EINVAL;
        break;
    }

    dummy_amp_changed(priv);
    priv->powerstat = status;

    return RIG_OK;
}


static int dummy_amp_get_powerstat(AMP *amp, powerstat_t *status)
{
    const struct dummy_amp_priv_data *priv = (struct dummy_amp_priv_data *)
            AMPSTATE(amp)->priv;

    *status = priv->powerstat;
    rig_debug(RIG_DEBUG_VERBOSE, "%s called\n", __func__);

    return RIG_OK;
}

static int dummy_set_func(AMP *amp, setting_t func, int status)
{
    struct dummy_amp_priv_data *priv = (struct dummy_amp_priv_data *)
                                       AMPSTATE(amp)->priv;

    rig_debug(RIG_DEBUG_VERBOSE, "%s called: %s %d\n", __func__,
              amp_strfunc(func), status);

    if (status)
    {
        priv->funcs |=  func;
    }
    else
    {
        priv->funcs &= ~func;
    }

    dummy_amp_changed(priv);

    return RIG_OK;
}

static int dummy_get_func(AMP *amp, setting_t func, int *status)
{
    const struct dummy_amp_priv_data *priv = (struct dummy_amp_priv_data *)
            AMPSTATE(amp)->priv;

    *status = (priv->funcs & func) ? 1 : 0;

    rig_debug(RIG_DEBUG_VERBOSE, "%s called: %s\n", __func__,
              amp_strfunc(func));

    return RIG_OK;
}

static int dummy_amp_op(AMP *amp, amp_op_t op)
{
    rig_debug(RIG_DEBUG_VERBOSE, "%s called: %s\n", __func__,
              amp_strampop(op));

    return RIG_OK;
}

/**
 * Dummy amplifier capabilities.
 */
const struct amp_caps dummy_amp_caps =
{
    AMP_MODEL(AMP_MODEL_DUMMY),
    .model_name =     "Dummy",
    .mfg_name =       "Hamlib",
    .version =        "20261024.0",
    .copyright =      "LGPL",
    .status =         RIG_STATUS_STABLE,
    .amp_type =       AMP_TYPE_OTHER,
    .port_type =      RIG_PORT_NONE,

    .priv =  NULL,  /* priv */

    .has_get_func = DUMMY_AMP_GET_FUNCS,
    .has_set_func = DUMMY_AMP_SET_FUNCS,
    .has_get_level = DUMMY_AMP_GET_LEVELS,
    .has_set_level = DUMMY_AMP_SET_LEVELS,
    .has_get_parm = DUMMY_AMP_GET_PARMS,
    .has_set_parm = DUMMY_AMP_SET_PARMS,

    .level_gran = {
        [AMP_LVL_PWR] = { .min = { .f = 0 }, .max = { .f = 1.0f }, .step = { .f = 0.01f } },
        /* Listed for the capabilities with no fixed values: messages, none while all is well */
        [AMP_LVL_FAULT] = { .step = { .s = "" } },
        [AMP_LVL_WARNING] = { .step = { .s = "" } },
    },

    .extlevels =    dummy_ext_levels,
    .extfuncs =     dummy_ext_funcs,
    .extparms =     dummy_ext_parms,
    .cfgparams =    dummy_cfg_params,

    .amp_ops = DUMMY_AMP_AMP_OPS,
    .has_status = AMP_STATUS_PTT,

    .amp_init =     dummy_amp_init,
    .amp_cleanup =  dummy_amp_cleanup,
    .amp_open =     dummy_amp_open,
    .amp_close =    dummy_amp_close,

    .set_conf =     dummy_set_conf,
    .get_conf =     dummy_get_conf,
    .get_conf2 =    dummy_get_conf2,

    .get_freq =     dummy_amp_get_freq,
    .set_freq =     dummy_amp_set_freq,
    .get_input =    dummy_amp_get_input,
    .set_input =    dummy_amp_set_input,
    .get_ant =      dummy_amp_get_ant,
    .set_ant =      dummy_amp_set_ant,

    .set_func = dummy_set_func,
    .get_func = dummy_get_func,
    .set_level = dummy_set_level,
    .get_level = dummy_get_level,
    .set_parm = dummy_set_parm,
    .get_parm = dummy_get_parm,

    .set_ext_func = dummy_set_ext_func,
    .get_ext_func = dummy_get_ext_func,
    .set_ext_level = dummy_set_ext_level,
    .get_ext_level = dummy_get_ext_level,
    .set_ext_parm = dummy_set_ext_parm,
    .get_ext_parm = dummy_get_ext_parm,

    .get_info = dummy_amp_get_info,
    .get_status = dummy_amp_get_status,

    .set_powerstat = dummy_amp_set_powerstat,
    .get_powerstat = dummy_amp_get_powerstat,

    .amp_op =   dummy_amp_op,
    .reset =    dummy_amp_reset,

    .range_list1 = {
        FRQ_RNG_HF(1, RIG_MODE_ALL, W(1), W(1500), DUMMY_AMP_INPUTS, DUMMY_AMP_ANTS),
        FRQ_RNG_6m(1, RIG_MODE_ALL, W(1), W(1500), DUMMY_AMP_INPUTS, DUMMY_AMP_ANTS),
        FRQ_RNG_60m(1, RIG_MODE_ALL, W(1), W(1500), DUMMY_AMP_INPUTS, DUMMY_AMP_ANTS),
        RIG_FRNG_END,
    },
    .range_list2 = {
        FRQ_RNG_HF(2, RIG_MODE_ALL, W(1), W(1500), DUMMY_AMP_INPUTS, DUMMY_AMP_ANTS),
        FRQ_RNG_6m(2, RIG_MODE_ALL, W(1), W(1500), DUMMY_AMP_INPUTS, DUMMY_AMP_ANTS),
        FRQ_RNG_60m(2, RIG_MODE_ALL, W(1), W(1500), DUMMY_AMP_INPUTS, DUMMY_AMP_ANTS),
        RIG_FRNG_END,
    },
};

DECLARE_INITAMP_BACKEND(dummy)
{
    rig_debug(RIG_DEBUG_VERBOSE, "%s: _init called\n", __func__);

    amp_register(&dummy_amp_caps);
    amp_register(&netampctl_caps);

    return RIG_OK;
}
