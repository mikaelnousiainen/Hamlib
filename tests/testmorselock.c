/*
 * Hamlib public calls while the Morse data handler keys the rig
 * Copyright (c) 2026 by Hamlib Team
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

/*
 * The Morse data handler sends the text rig_send_morse() queues from a thread
 * of its own, holding the API lock while it does. A public call that talks to
 * the rig must hold the lock too, or its exchange with the rig runs beside the
 * handler's and each reads the other's replies.
 *
 * The dummy rig's send_morse is replaced by one that keys for 50 ms, and the
 * backend function of each call by one that notes whether the handler was
 * keying meanwhile. For each call a character is queued and, once the handler
 * keys it, the call is made: its backend function must not run while the
 * handler keys, and the call must not return before the handler is done. The
 * generic channel calls put the rig on a memory channel and back, and the
 * handler must not key while the rig is on the channel.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "hamlib/rig.h"
#include "hamlib/riglist.h"

#define TEST_CHANNEL 7

static volatile int keying;           /* the handler is in send_morse */
static volatile int sends;            /* the sends started */
static volatile int saw_keying;       /* a backend function ran while the rig was keyed */
static volatile int reached;          /* backend functions run during the case */
static volatile int on_channel;       /* the rig is on TEST_CHANNEL */
static volatile int keyed_on_channel; /* the rig was keyed while on TEST_CHANNEL */
static volatile int key_on_channel;   /* set_mem queues text when it puts the rig on TEST_CHANNEL */
static RIG *test_rig;
static int (*dummy_set_vfo)(RIG *rig, vfo_t vfo);
static int (*dummy_get_freq)(RIG *rig, vfo_t vfo, freq_t *freq);
static const struct confparams *dummy_extlevels;

static int get(volatile int *value)
{
    return __atomic_load_n(value, __ATOMIC_SEQ_CST);
}

static void set(volatile int *value, int to)
{
    __atomic_store_n(value, to, __ATOMIC_SEQ_CST);
}

static void add(volatile int *value)
{
    __atomic_add_fetch(value, 1, __ATOMIC_SEQ_CST);
}

static int keying_send_morse(RIG *rig, vfo_t vfo, const char *msg)
{
    (void)rig;
    (void)vfo;
    (void)msg;

    if (get(&keying))
    {
        set(&saw_keying, 1);
    }

    if (get(&on_channel))
    {
        set(&keyed_on_channel, 1);
    }

    set(&keying, 1);
    add(&sends);
    hl_usleep(50 * 1000);
    set(&keying, 0);
    return RIG_OK;
}

/* What every backend function below does: notes whether the rig was keyed */
static int backend(void)
{
    add(&reached);

    if (get(&keying))
    {
        set(&saw_keying, 1);
    }

    return RIG_OK;
}

static int cw_get_mode(RIG *r, vfo_t v, rmode_t *m, pbwidth_t *w)
{
    (void)r; (void)v; *m = RIG_MODE_CW; *w = 500; return RIG_OK;
}

static int stub_get_dcd(RIG *r, vfo_t v, dcd_t *d)
{
    (void)r; (void)v; *d = RIG_DCD_OFF; return backend();
}

static int stub_set_rptr_shift(RIG *r, vfo_t v, rptr_shift_t s)
{
    (void)r; (void)v; (void)s; return backend();
}

static int stub_get_rptr_shift(RIG *r, vfo_t v, rptr_shift_t *s)
{
    (void)r; (void)v; *s = RIG_RPT_SHIFT_NONE; return backend();
}

static int stub_set_shortfreq(RIG *r, vfo_t v, shortfreq_t f)
{
    (void)r; (void)v; (void)f; return backend();
}

static int stub_get_shortfreq(RIG *r, vfo_t v, shortfreq_t *f)
{
    (void)r; (void)v; *f = 0; return backend();
}

static int stub_set_split_freq(RIG *r, vfo_t v, freq_t f)
{
    (void)r; (void)v; (void)f; return backend();
}

static int stub_get_split_freq(RIG *r, vfo_t v, freq_t *f)
{
    (void)r; (void)v; *f = 14075000; return backend();
}

static int stub_set_split_mode(RIG *r, vfo_t v, rmode_t m, pbwidth_t w)
{
    (void)r; (void)v; (void)m; (void)w; return backend();
}

static int stub_get_split_mode(RIG *r, vfo_t v, rmode_t *m, pbwidth_t *w)
{
    (void)r; (void)v; *m = RIG_MODE_CW; *w = 500; return backend();
}

static int stub_set_split_freq_mode(RIG *r, vfo_t v, freq_t f, rmode_t m,
                                    pbwidth_t w)
{
    (void)r; (void)v; (void)f; (void)m; (void)w; return backend();
}

static int stub_get_split_freq_mode(RIG *r, vfo_t v, freq_t *f, rmode_t *m,
                                    pbwidth_t *w)
{
    (void)r; (void)v; *f = 14075000; *m = RIG_MODE_CW; *w = 500;
    return backend();
}

static int stub_set_vfo(RIG *r, vfo_t v)
{
    (void)r; (void)v; return backend();
}

static int stub_get_freq(RIG *r, vfo_t v, freq_t *f)
{
    (void)r; (void)v; *f = 14074000; return backend();
}

static int stub_set_split_vfo(RIG *r, vfo_t v, split_t s, vfo_t tx)
{
    (void)r; (void)v; (void)s; (void)tx; return backend();
}

static int stub_get_split_vfo(RIG *r, vfo_t v, split_t *s, vfo_t *tx)
{
    (void)r; (void)v; *s = RIG_SPLIT_OFF; *tx = RIG_VFO_A; return backend();
}

static int stub_set_ant(RIG *r, vfo_t v, ant_t a, value_t o)
{
    (void)r; (void)v; (void)a; (void)o; return backend();
}

static int stub_get_ant(RIG *r, vfo_t v, ant_t a, value_t *o, ant_t *curr,
                        ant_t *tx, ant_t *rx)
{
    (void)r; (void)v; (void)a; o->i = 0; *curr = *tx = *rx = RIG_ANT_1;
    return backend();
}

static int stub_power2mW(RIG *r, unsigned int *mw, float p, freq_t f,
                         rmode_t m)
{
    (void)r; (void)f; (void)m; *mw = (unsigned int)(p * 100000); return backend();
}

static int stub_mW2power(RIG *r, float *p, unsigned int mw, freq_t f,
                         rmode_t m)
{
    (void)r; (void)f; (void)m; *p = mw / 100000.0f; return backend();
}

static int stub_set_powerstat(RIG *r, powerstat_t s)
{
    (void)r; (void)s; return backend();
}

static int stub_get_powerstat(RIG *r, powerstat_t *s)
{
    (void)r; *s = RIG_POWER_ON; return backend();
}

static int stub_reset(RIG *r, reset_t reset)
{
    (void)r; (void)reset; return backend();
}

static int stub_vfo_op(RIG *r, vfo_t v, vfo_op_t op)
{
    (void)r; (void)v; (void)op; return backend();
}

static int stub_scan(RIG *r, vfo_t v, scan_t s, int ch)
{
    (void)r; (void)v; (void)s; (void)ch; return backend();
}

static int stub_send_dtmf(RIG *r, vfo_t v, const char *digits)
{
    (void)r; (void)v; (void)digits; return backend();
}

static int stub_recv_dtmf(RIG *r, vfo_t v, char *digits, int *length)
{
    (void)r; (void)v; digits[0] = '\0'; *length = 0; return backend();
}

static int stub_send_voice_mem(RIG *r, vfo_t v, int ch)
{
    (void)r; (void)v; (void)ch; return backend();
}

static int stub_stop_voice_mem(RIG *r, vfo_t v)
{
    (void)r; (void)v; return backend();
}

static int stub_set_int(RIG *r, int value)
{
    (void)r; (void)value; return backend();
}

static int stub_get_int(RIG *r, int *value)
{
    (void)r; *value = 0; return backend();
}

static const char *stub_get_info(RIG *r)
{
    (void)r; backend(); return "stub";
}

static int stub_set_clock(RIG *r, int year, int month, int day, int hour,
                          int min, int sec, double msec, int utc_offset)
{
    (void)r; (void)year; (void)month; (void)day; (void)hour; (void)min;
    (void)sec; (void)msec; (void)utc_offset;
    return backend();
}

static int stub_get_clock(RIG *r, int *year, int *month, int *day, int *hour,
                          int *min, int *sec, double *msec, int *utc_offset)
{
    (void)r;
    *year = 2026; *month = 10; *day = 31; *hour = *min = *sec = 0;
    *msec = 0; *utc_offset = 0;
    return backend();
}

static int stub_password(RIG *r, const char *key)
{
    (void)r; (void)key; return backend();
}

static int stub_set_func(RIG *r, vfo_t v, setting_t f, int status)
{
    (void)r; (void)v; (void)f; (void)status; return backend();
}

static int stub_get_func(RIG *r, vfo_t v, setting_t f, int *status)
{
    (void)r; (void)v; (void)f; *status = 0; return backend();
}

static int stub_set_parm(RIG *r, setting_t p, value_t val)
{
    (void)r; (void)p; (void)val; return backend();
}

static int stub_get_parm(RIG *r, setting_t p, value_t *val)
{
    (void)r; (void)p; val->i = 0; return backend();
}

static int stub_set_ext_level(RIG *r, vfo_t v, hamlib_token_t t, value_t val)
{
    (void)r; (void)v; (void)t; (void)val; return backend();
}

static int stub_get_ext_level(RIG *r, vfo_t v, hamlib_token_t t, value_t *val)
{
    (void)r; (void)v; (void)t; val->i = 0; return backend();
}

static int stub_set_ext_func(RIG *r, vfo_t v, hamlib_token_t t, int status)
{
    (void)r; (void)v; (void)t; (void)status; return backend();
}

static int stub_get_ext_func(RIG *r, vfo_t v, hamlib_token_t t, int *status)
{
    (void)r; (void)v; (void)t; *status = 0; return backend();
}

static int stub_set_ext_parm(RIG *r, hamlib_token_t t, value_t val)
{
    (void)r; (void)t; (void)val; return backend();
}

static int stub_get_ext_parm(RIG *r, hamlib_token_t t, value_t *val)
{
    (void)r; (void)t; val->i = 0; return backend();
}

/* Puts the rig on a memory channel; on TEST_CHANNEL, queues text to key there if asked to */
static int stub_set_mem(RIG *r, vfo_t v, int ch)
{
    (void)r; (void)v;
    set(&on_channel, ch == TEST_CHANNEL);

    if (ch == TEST_CHANNEL && get(&key_on_channel))
    {
        set(&key_on_channel, 0);
        rig_send_morse(test_rig, RIG_VFO_CURR, "E");
        /* longer than the handler waits between looks at its queue */
        hl_usleep(300 * 1000);
    }

    return backend();
}

static int stub_get_mem(RIG *r, vfo_t v, int *ch)
{
    (void)r; (void)v; *ch = get(&on_channel) ? TEST_CHANNEL : 0; return backend();
}

static int stub_set_bank(RIG *r, vfo_t v, int bank)
{
    (void)r; (void)v; (void)bank; return backend();
}

static int stub_set_channel(RIG *r, vfo_t v, const channel_t *chan)
{
    (void)r; (void)v; (void)chan; return backend();
}

static int stub_get_channel(RIG *r, vfo_t v, channel_t *chan, int read_only)
{
    (void)r; (void)v; (void)read_only; chan->freq = 14074000; return backend();
}

static int stub_chan_all_cb(RIG *r, vfo_t v, chan_cb_t cb, rig_ptr_t arg)
{
    (void)r; (void)v; (void)cb; (void)arg; return backend();
}

static int stub_mem_all_cb(RIG *r, vfo_t v, chan_cb_t cb, confval_cb_t parm_cb,
                           rig_ptr_t arg)
{
    (void)r; (void)v; (void)cb; (void)parm_cb; (void)arg; return backend();
}

static int stub_set_tone(RIG *r, vfo_t v, tone_t tone)
{
    (void)r; (void)v; (void)tone; return backend();
}

static int stub_get_tone(RIG *r, vfo_t v, tone_t *tone)
{
    (void)r; (void)v; *tone = 0; return backend();
}

static void install_stubs(struct rig_caps *caps)
{
    caps->send_morse = keying_send_morse;
    caps->get_mode = cw_get_mode;
    caps->get_dcd = stub_get_dcd;
    caps->set_rptr_shift = stub_set_rptr_shift;
    caps->get_rptr_shift = stub_get_rptr_shift;
    caps->set_rptr_offs = stub_set_shortfreq;
    caps->get_rptr_offs = stub_get_shortfreq;
    caps->set_split_freq = stub_set_split_freq;
    caps->get_split_freq = stub_get_split_freq;
    caps->set_split_mode = stub_set_split_mode;
    caps->get_split_mode = stub_get_split_mode;
    caps->set_split_freq_mode = stub_set_split_freq_mode;
    caps->get_split_freq_mode = stub_get_split_freq_mode;
    caps->set_split_vfo = stub_set_split_vfo;
    caps->get_split_vfo = stub_get_split_vfo;
    caps->set_rit = stub_set_shortfreq;
    caps->get_rit = stub_get_shortfreq;
    caps->set_xit = stub_set_shortfreq;
    caps->get_xit = stub_get_shortfreq;
    caps->set_ts = stub_set_shortfreq;
    caps->get_ts = stub_get_shortfreq;
    caps->set_ant = stub_set_ant;
    caps->get_ant = stub_get_ant;
    caps->power2mW = stub_power2mW;
    caps->mW2power = stub_mW2power;
    caps->set_powerstat = stub_set_powerstat;
    caps->get_powerstat = stub_get_powerstat;
    caps->reset = stub_reset;
    caps->vfo_op = stub_vfo_op;
    caps->scan = stub_scan;
    caps->send_dtmf = stub_send_dtmf;
    caps->recv_dtmf = stub_recv_dtmf;
    caps->send_voice_mem = stub_send_voice_mem;
    caps->stop_voice_mem = stub_stop_voice_mem;
    caps->set_vfo_opt = stub_set_int;
    caps->get_info = stub_get_info;
    caps->set_clock = stub_set_clock;
    caps->get_clock = stub_get_clock;
    caps->password = stub_password;
    caps->set_lock_mode = stub_set_int;
    caps->get_lock_mode = stub_get_int;
    caps->set_func = stub_set_func;
    caps->get_func = stub_get_func;
    caps->set_parm = stub_set_parm;
    caps->get_parm = stub_get_parm;
    caps->set_ext_level = stub_set_ext_level;
    caps->get_ext_level = stub_get_ext_level;
    caps->set_ext_func = stub_set_ext_func;
    caps->get_ext_func = stub_get_ext_func;
    caps->set_ext_parm = stub_set_ext_parm;
    caps->get_ext_parm = stub_get_ext_parm;
    caps->set_mem = stub_set_mem;
    caps->get_mem = stub_get_mem;
    caps->set_bank = stub_set_bank;
    caps->set_channel = stub_set_channel;
    caps->get_channel = stub_get_channel;
    caps->set_chan_all_cb = stub_chan_all_cb;
    caps->get_chan_all_cb = stub_chan_all_cb;
    caps->set_mem_all_cb = stub_mem_all_cb;
    caps->get_mem_all_cb = stub_mem_all_cb;
    caps->set_ctcss_tone = stub_set_tone;
    caps->get_ctcss_tone = stub_get_tone;
    caps->set_dcs_code = stub_set_tone;
    caps->get_dcs_code = stub_get_tone;
    caps->set_ctcss_sql = stub_set_tone;
    caps->get_ctcss_sql = stub_get_tone;
    caps->set_dcs_sql = stub_set_tone;
    caps->get_dcs_sql = stub_get_tone;
}

static channel_t test_chan;
static channel_t test_chans[2];
static value_t parm_values[1];

static int call_get_dcd(RIG *rig) { dcd_t d; return rig_get_dcd(rig, RIG_VFO_CURR, &d); }
static int call_set_rptr_shift(RIG *rig) { return rig_set_rptr_shift(rig, RIG_VFO_CURR, RIG_RPT_SHIFT_NONE); }
static int call_get_rptr_shift(RIG *rig) { rptr_shift_t s; return rig_get_rptr_shift(rig, RIG_VFO_CURR, &s); }
static int call_set_rptr_offs(RIG *rig) { return rig_set_rptr_offs(rig, RIG_VFO_CURR, 600000); }
static int call_get_rptr_offs(RIG *rig) { shortfreq_t o; return rig_get_rptr_offs(rig, RIG_VFO_CURR, &o); }
static int call_set_split_freq(RIG *rig) { return rig_set_split_freq(rig, RIG_VFO_CURR, 14075000); }
static int call_get_split_freq(RIG *rig) { freq_t f; return rig_get_split_freq(rig, RIG_VFO_CURR, &f); }
static int call_set_split_mode(RIG *rig) { return rig_set_split_mode(rig, RIG_VFO_CURR, RIG_MODE_CW, 500); }
static int call_get_split_mode(RIG *rig) { rmode_t m; pbwidth_t w; return rig_get_split_mode(rig, RIG_VFO_CURR, &m, &w); }
static int call_set_split_freq_mode(RIG *rig) { return rig_set_split_freq_mode(rig, RIG_VFO_CURR, 14075000, RIG_MODE_CW, 500); }
static int call_get_split_freq_mode(RIG *rig) { freq_t f; rmode_t m; pbwidth_t w; return rig_get_split_freq_mode(rig, RIG_VFO_CURR, &f, &m, &w); }
static int call_set_split_vfo(RIG *rig) { return rig_set_split_vfo(rig, RIG_VFO_CURR, RIG_SPLIT_OFF, RIG_VFO_A); }
static int call_get_split_vfo(RIG *rig) { split_t s; vfo_t tx; return rig_get_split_vfo(rig, RIG_VFO_CURR, &s, &tx); }
static int call_set_rit(RIG *rig) { return rig_set_rit(rig, RIG_VFO_CURR, 0); }
static int call_get_rit(RIG *rig) { shortfreq_t f; return rig_get_rit(rig, RIG_VFO_CURR, &f); }
static int call_set_xit(RIG *rig) { return rig_set_xit(rig, RIG_VFO_CURR, 0); }
static int call_get_xit(RIG *rig) { shortfreq_t f; return rig_get_xit(rig, RIG_VFO_CURR, &f); }
static int call_set_ts(RIG *rig) { return rig_set_ts(rig, RIG_VFO_CURR, 100); }
static int call_get_ts(RIG *rig) { shortfreq_t f; return rig_get_ts(rig, RIG_VFO_CURR, &f); }
static int call_set_ant(RIG *rig) { value_t o; o.i = 0; return rig_set_ant(rig, RIG_VFO_CURR, RIG_ANT_1, o); }
static int call_get_ant(RIG *rig) { value_t o; ant_t c, t, r; return rig_get_ant(rig, RIG_VFO_CURR, RIG_ANT_CURR, &o, &c, &t, &r); }
static int call_power2mW(RIG *rig) { unsigned int mw; return rig_power2mW(rig, &mw, 0.5f, 14074000, RIG_MODE_CW); }
static int call_mW2power(RIG *rig) { float p; return rig_mW2power(rig, &p, 50000, 14074000, RIG_MODE_CW); }
static int call_set_powerstat(RIG *rig) { return rig_set_powerstat(rig, RIG_POWER_ON); }
static int call_get_powerstat(RIG *rig) { powerstat_t s; return rig_get_powerstat(rig, &s); }
static int call_reset(RIG *rig) { return rig_reset(rig, RIG_RESET_SOFT); }
static int call_vfo_op(RIG *rig) { return rig_vfo_op(rig, RIG_VFO_CURR, RIG_OP_CPY); }
static int call_scan(RIG *rig) { return rig_scan(rig, RIG_VFO_CURR, RIG_SCAN_STOP, 0); }
static int call_send_dtmf(RIG *rig) { return rig_send_dtmf(rig, RIG_VFO_CURR, "1"); }
static int call_recv_dtmf(RIG *rig) { char d[8]; int n = sizeof(d) - 1; return rig_recv_dtmf(rig, RIG_VFO_CURR, d, &n); }
static int call_send_morse_other_vfo(RIG *rig) { return rig_send_morse(rig, RIG_VFO_B, "E"); }
static int call_send_voice_mem(RIG *rig) { return rig_send_voice_mem(rig, RIG_VFO_CURR, 1); }
static int call_stop_voice_mem(RIG *rig) { return rig_stop_voice_mem(rig, RIG_VFO_CURR); }
static int call_set_vfo_opt(RIG *rig) { return rig_set_vfo_opt(rig, 0); }
static int call_get_info(RIG *rig) { return rig_get_info(rig) != NULL ? RIG_OK : -RIG_EINTERNAL; }
static int call_set_clock(RIG *rig) { return rig_set_clock(rig, 2026, 10, 31, 0, 0, 0, 0, 0); }
static int call_get_clock(RIG *rig) { int y, mo, d, h, mi, s, u; double ms; return rig_get_clock(rig, &y, &mo, &d, &h, &mi, &s, &ms, &u); }
static int call_password(RIG *rig) { return rig_password(rig, "key"); }
static int call_send_raw(RIG *rig) { return rig_send_raw(rig, (const unsigned char *)"E", 1, NULL, 0, NULL); }
static int call_set_lock_mode(RIG *rig) { return rig_set_lock_mode(rig, 0); }
static int call_get_lock_mode(RIG *rig) { int m; return rig_get_lock_mode(rig, &m); }
static int call_set_parm(RIG *rig) { value_t v; v.i = 1; return rig_set_parm(rig, RIG_PARM_BEEP, v); }
static int call_get_parm(RIG *rig) { value_t v; return rig_get_parm(rig, RIG_PARM_BEEP, &v); }
static int call_set_func(RIG *rig) { return rig_set_func(rig, RIG_VFO_CURR, RIG_FUNC_NB, 1); }
static int call_get_func(RIG *rig) { int s; return rig_get_func(rig, RIG_VFO_CURR, RIG_FUNC_NB, &s); }
static int call_set_ext_level(RIG *rig) { value_t v; v.i = 1; return rig_set_ext_level(rig, RIG_VFO_CURR, rig_ext_token_lookup(rig, "MGL"), v); }
static int call_get_ext_level(RIG *rig) { value_t v; return rig_get_ext_level(rig, RIG_VFO_CURR, rig_ext_token_lookup(rig, "MGL"), &v); }
static int call_set_ext_func(RIG *rig) { return rig_set_ext_func(rig, RIG_VFO_CURR, rig_ext_token_lookup(rig, "MGF"), 1); }
static int call_get_ext_func(RIG *rig) { int s; return rig_get_ext_func(rig, RIG_VFO_CURR, rig_ext_token_lookup(rig, "MGF"), &s); }
static int call_set_ext_parm(RIG *rig) { value_t v; v.i = 1; return rig_set_ext_parm(rig, rig_ext_token_lookup(rig, "MGP"), v); }
static int call_get_ext_parm(RIG *rig) { value_t v; return rig_get_ext_parm(rig, rig_ext_token_lookup(rig, "MGP"), &v); }
static int call_set_mem(RIG *rig) { return rig_set_mem(rig, RIG_VFO_CURR, 1); }
static int call_get_mem(RIG *rig) { int ch; return rig_get_mem(rig, RIG_VFO_CURR, &ch); }
static int call_set_bank(RIG *rig) { return rig_set_bank(rig, RIG_VFO_CURR, 1); }
static int call_set_channel(RIG *rig) { test_chan.vfo = RIG_VFO_MEM; test_chan.channel_num = 1; return rig_set_channel(rig, RIG_VFO_CURR, &test_chan); }
static int call_get_channel(RIG *rig) { test_chan.vfo = RIG_VFO_MEM; test_chan.channel_num = 1; return rig_get_channel(rig, RIG_VFO_CURR, &test_chan, 0); }
static int call_set_chan_all(RIG *rig) { return rig_set_chan_all(rig, RIG_VFO_CURR, test_chans); }
static int call_get_chan_all(RIG *rig) { return rig_get_chan_all(rig, RIG_VFO_CURR, test_chans); }
static int no_channel(RIG *rig, vfo_t vfo, channel_t **chan, int ch, const chan_t *chan_list, rig_ptr_t arg)
{
    (void)rig; (void)vfo; (void)chan; (void)ch; (void)chan_list; (void)arg; return RIG_OK;
}
static int call_set_chan_all_cb(RIG *rig) { return rig_set_chan_all_cb(rig, RIG_VFO_CURR, no_channel, NULL); }
static int call_get_chan_all_cb(RIG *rig) { return rig_get_chan_all_cb(rig, RIG_VFO_CURR, no_channel, NULL); }
static int no_parm(RIG *rig, const struct confparams *parm, value_t *value, rig_ptr_t arg)
{
    (void)rig; (void)parm; (void)value; (void)arg; return RIG_OK;
}
static int call_set_mem_all_cb(RIG *rig) { return rig_set_mem_all_cb(rig, RIG_VFO_CURR, no_channel, no_parm, NULL); }
static int call_get_mem_all_cb(RIG *rig) { return rig_get_mem_all_cb(rig, RIG_VFO_CURR, no_channel, no_parm, NULL); }
static int call_set_mem_all(RIG *rig) { return rig_set_mem_all(rig, RIG_VFO_CURR, test_chans, rig->caps->extparms, parm_values); }
static int call_get_mem_all(RIG *rig) { return rig_get_mem_all(rig, RIG_VFO_CURR, test_chans, rig->caps->extparms, parm_values); }
static int call_set_ctcss_tone(RIG *rig) { return rig_set_ctcss_tone(rig, RIG_VFO_CURR, 885); }
static int call_get_ctcss_tone(RIG *rig) { tone_t t; return rig_get_ctcss_tone(rig, RIG_VFO_CURR, &t); }
static int call_set_dcs_code(RIG *rig) { return rig_set_dcs_code(rig, RIG_VFO_CURR, 23); }
static int call_get_dcs_code(RIG *rig) { tone_t t; return rig_get_dcs_code(rig, RIG_VFO_CURR, &t); }
static int call_set_ctcss_sql(RIG *rig) { return rig_set_ctcss_sql(rig, RIG_VFO_CURR, 885); }
static int call_get_ctcss_sql(RIG *rig) { tone_t t; return rig_get_ctcss_sql(rig, RIG_VFO_CURR, &t); }
static int call_set_dcs_sql(RIG *rig) { return rig_set_dcs_sql(rig, RIG_VFO_CURR, 23); }
static int call_get_dcs_sql(RIG *rig) { tone_t t; return rig_get_dcs_sql(rig, RIG_VFO_CURR, &t); }

struct lock_case
{
    const char *name;
    int (*call)(RIG *rig);
    int reaches_backend;    /* whether one of the stubs above answers it */
    int answers_cached;     /* while the handler keys, it may answer from the cache instead */
};

static const struct lock_case cases[] =
{
    { "rig_get_dcd", call_get_dcd, 1 },
    { "rig_set_rptr_shift", call_set_rptr_shift, 1 },
    { "rig_get_rptr_shift", call_get_rptr_shift, 1 },
    { "rig_set_rptr_offs", call_set_rptr_offs, 1 },
    { "rig_get_rptr_offs", call_get_rptr_offs, 1 },
    { "rig_set_split_freq", call_set_split_freq, 1 },
    { "rig_get_split_freq", call_get_split_freq, 1 },
    { "rig_set_split_mode", call_set_split_mode, 1 },
    { "rig_get_split_mode", call_get_split_mode, 1 },
    { "rig_set_split_freq_mode", call_set_split_freq_mode, 1 },
    { "rig_get_split_freq_mode", call_get_split_freq_mode, 1 },
    { "rig_set_split_vfo", call_set_split_vfo, 1 },
    { "rig_get_split_vfo", call_get_split_vfo, 0, 1 },
    { "rig_set_rit", call_set_rit, 1 },
    { "rig_get_rit", call_get_rit, 1 },
    { "rig_set_xit", call_set_xit, 1 },
    { "rig_get_xit", call_get_xit, 1 },
    { "rig_set_ts", call_set_ts, 1 },
    { "rig_get_ts", call_get_ts, 1 },
    { "rig_set_ant", call_set_ant, 1 },
    { "rig_get_ant", call_get_ant, 1 },
    { "rig_power2mW", call_power2mW, 1 },
    { "rig_mW2power", call_mW2power, 1 },
    { "rig_set_powerstat", call_set_powerstat, 1 },
    { "rig_get_powerstat", call_get_powerstat, 1 },
    { "rig_reset", call_reset, 1 },
    { "rig_vfo_op", call_vfo_op, 1 },
    { "rig_scan", call_scan, 1 },
    { "rig_send_dtmf", call_send_dtmf, 1 },
    { "rig_recv_dtmf", call_recv_dtmf, 1 },
    { "rig_send_morse on another VFO", call_send_morse_other_vfo, 0 },
    { "rig_send_voice_mem", call_send_voice_mem, 1 },
    { "rig_stop_voice_mem", call_stop_voice_mem, 1 },
    { "rig_set_vfo_opt", call_set_vfo_opt, 1 },
    { "rig_get_info", call_get_info, 1 },
    { "rig_set_clock", call_set_clock, 1 },
    { "rig_get_clock", call_get_clock, 1 },
    { "rig_password", call_password, 1 },
    { "rig_send_raw", call_send_raw, 0 },
    { "rig_set_lock_mode", call_set_lock_mode, 1 },
    { "rig_get_lock_mode", call_get_lock_mode, 1 },
    { "rig_set_parm", call_set_parm, 1 },
    { "rig_get_parm", call_get_parm, 1 },
    { "rig_set_func", call_set_func, 1 },
    { "rig_get_func", call_get_func, 1 },
    { "rig_set_ext_level", call_set_ext_level, 1 },
    { "rig_get_ext_level", call_get_ext_level, 1 },
    { "rig_set_ext_func", call_set_ext_func, 1 },
    { "rig_get_ext_func", call_get_ext_func, 1 },
    { "rig_set_ext_parm", call_set_ext_parm, 1 },
    { "rig_get_ext_parm", call_get_ext_parm, 1 },
    { "rig_set_mem", call_set_mem, 1 },
    { "rig_get_mem", call_get_mem, 1 },
    { "rig_set_bank", call_set_bank, 1 },
    { "rig_set_channel", call_set_channel, 1 },
    { "rig_get_channel", call_get_channel, 1 },
    { "rig_set_chan_all", call_set_chan_all, 1 },
    { "rig_get_chan_all", call_get_chan_all, 1 },
    { "rig_set_chan_all_cb", call_set_chan_all_cb, 1 },
    { "rig_get_chan_all_cb", call_get_chan_all_cb, 1 },
    { "rig_set_mem_all_cb", call_set_mem_all_cb, 1 },
    { "rig_get_mem_all_cb", call_get_mem_all_cb, 1 },
    { "rig_set_mem_all", call_set_mem_all, 1 },
    { "rig_get_mem_all", call_get_mem_all, 1 },
    { "rig_set_ctcss_tone", call_set_ctcss_tone, 1 },
    { "rig_get_ctcss_tone", call_get_ctcss_tone, 1 },
    { "rig_set_dcs_code", call_set_dcs_code, 1 },
    { "rig_get_dcs_code", call_get_dcs_code, 1 },
    { "rig_set_ctcss_sql", call_set_ctcss_sql, 1 },
    { "rig_get_ctcss_sql", call_get_ctcss_sql, 1 },
    { "rig_set_dcs_sql", call_set_dcs_sql, 1 },
    { "rig_get_dcs_sql", call_get_dcs_sql, 1 },
};

/* Waits up to two seconds for the value to become the one wanted */
static int wait_for(volatile int *value, int wanted)
{
    int i;

    for (i = 0; i < 2000 && get(value) != wanted; i++)
    {
        hl_usleep(1000);
    }

    return get(value) == wanted;
}

/* Queues a character, and waits for the handler to key it */
static int key_one(RIG *rig)
{
    int i, started = get(&sends);

    if (rig_send_morse(rig, RIG_VFO_CURR, "E") != RIG_OK)
    {
        return 0;
    }

    for (i = 0; i < 2000 && !(get(&keying) && get(&sends) > started); i++)
    {
        hl_usleep(1000);
    }

    return get(&keying) && get(&sends) > started;
}

static int run_case(RIG *rig, const struct lock_case *c)
{
    int retval, returned_keying;

    if (!key_one(rig))
    {
        printf("%s: the Morse data handler did not key the queued text\n",
                c->name);
        return 1;
    }

    set(&saw_keying, 0);
    set(&reached, 0);
    retval = c->call(rig);
    returned_keying = get(&keying);

    if (!wait_for(&keying, 0))
    {
        printf("%s: the Morse data handler kept keying\n", c->name);
        return 1;
    }

    if (c->reaches_backend && get(&reached) == 0)
    {
        printf("%s: the call did not reach the rig (%s)\n", c->name,
                rigerror2(retval));
        return 1;
    }

    if (get(&saw_keying) || (returned_keying && !c->answers_cached))
    {
        printf("FAIL: %s ran while the Morse data handler keyed the rig\n", c->name);
        return 1;
    }

    printf("PASS: %s waits for the Morse data handler\n", c->name);
    return 0;
}

/* The generic channel calls, the backend's own taken away: the rig is not keyed on the channel */
static int run_channel_case(RIG *rig, const char *name, int set_it)
{
    int retval;

    rig->caps->set_channel = NULL;
    rig->caps->get_channel = NULL;
    /* the dummy rig itself has no memory VFO to read */
    rig->caps->set_vfo = stub_set_vfo;
    rig->caps->get_freq = stub_get_freq;
    /* a channel's extension levels are saved one entry short when there are more than one */
    rig->caps->extlevels = NULL;
    memset(&test_chan, 0, sizeof(test_chan));
    test_chan.vfo = RIG_VFO_MEM;
    test_chan.channel_num = TEST_CHANNEL;
    set(&keyed_on_channel, 0);
    set(&key_on_channel, 1);
    retval = set_it ? rig_set_channel(rig, RIG_VFO_CURR, &test_chan)
             : rig_get_channel(rig, RIG_VFO_CURR, &test_chan, 0);
    rig->caps->set_channel = stub_set_channel;
    rig->caps->get_channel = stub_get_channel;
    rig->caps->set_vfo = dummy_set_vfo;
    rig->caps->get_freq = dummy_get_freq;
    rig->caps->extlevels = dummy_extlevels;

    if (get(&key_on_channel) || get(&on_channel))
    {
        printf("%s: the rig was not put on channel %d and back (%s)\n", name,
                TEST_CHANNEL, rigerror2(retval));
        return 1;
    }

    /* the text queued on the channel is keyed once the call is done */
    hl_usleep(300 * 1000);

    if (!wait_for(&keying, 0))
    {
        printf("%s: the Morse data handler kept keying\n", name);
        return 1;
    }

    if (get(&keyed_on_channel))
    {
        printf("FAIL: %s let the Morse data handler key the rig on channel %d\n", name,
               TEST_CHANNEL);
        return 1;
    }

    printf("PASS: %s keeps the Morse data handler off channel %d\n", name, TEST_CHANNEL);
    return 0;
}

int main(void)
{
    RIG *rig;
    size_t i;
    int failures = 0;

    rig_set_debug(RIG_DEBUG_NONE);
    setvbuf(stdout, NULL, _IOLBF, 0);

    /* rig_set_func() runs a hamlib_tuner_control it finds in the working directory, as the tests directory has */
    if (chdir("/") != 0)
    {
        printf("failed to leave the working directory\n");
        return 1;
    }

    if (rig_load_backend("dummy") != RIG_OK)
    {
        printf("failed to load the dummy backend\n");
        return 1;
    }

    rig = rig_init(RIG_MODEL_DUMMY);

    if (rig == NULL)
    {
        printf("failed to initialize the Dummy rig\n");
        return 1;
    }

    test_rig = rig;
    dummy_set_vfo = rig->caps->set_vfo;
    dummy_get_freq = rig->caps->get_freq;
    dummy_extlevels = rig->caps->extlevels;
    install_stubs(rig->caps);

    if (rig_open(rig) != RIG_OK)
    {
        printf("failed to open the Dummy rig\n");
        return 1;
    }

    rig_set_cache_timeout_ms(rig, HAMLIB_CACHE_ALL, 0);

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        failures += run_case(rig, &cases[i]);
    }

    failures += run_channel_case(rig, "rig_get_channel on a memory channel", 0);
    failures += run_channel_case(rig, "rig_set_channel on a memory channel", 1);

    /* the lock is free after the last call: the handler still keys */
    if (!key_one(rig) || !wait_for(&keying, 0))
    {
        printf("the Morse data handler did not key after the calls\n");
        failures++;
    }

    rig_close(rig);
    rig_cleanup(rig);

    if (failures > 0)
    {
        printf("%d calls ran while the Morse data handler keyed the rig\n", failures);
        return 1;
    }

    return 0;
}
