/*
 *  Hamlib Expert amplifier backend - low level communication routines
 *  Copyright (c) 2023 by Michael Black W9MDB
 *  Copyright (c) 2024 by Mikael Nousiainen OH3BHX
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

#include "expert.h"
#include "register.h"
#include "misc.h"
#include "bandplan.h"

#define EXPERT_ANTS (RIG_ANT_1 | RIG_ANT_2 | RIG_ANT_3 | RIG_ANT_4)

#define EXPERT_AMP_OPS (AMP_OP_TUNE | AMP_OP_BAND_UP | AMP_OP_BAND_DOWN | AMP_OP_L_NH_UP | AMP_OP_L_NH_DOWN | AMP_OP_C_PF_UP | AMP_OP_C_PF_DOWN)

#define EXPERT_GET_FUNCS (AMP_FUNC_TUNER)
#define EXPERT_SET_FUNCS (0)
#define EXPERT_GET_LEVELS (AMP_LEVEL_SWR | AMP_LEVEL_SWR_TUNER | AMP_LEVEL_RFPOWER | AMP_LEVEL_PWR_FWD | AMP_LEVEL_PWR_PEAK | AMP_LEVEL_FAULT | AMP_LEVEL_WARNING | AMP_LEVEL_VD_METER | AMP_LEVEL_ID_METER)
#define EXPERT_SET_LEVELS (AMP_LEVEL_RFPOWER)
#define EXPERT_GET_PARMS (0)
#define EXPERT_SET_PARMS (AMP_PARM_BACKLIGHT)

#define EXPERT_AMP_COMMAND_INPUT 0x01
#define EXPERT_AMP_COMMAND_BAND_DOWN 0x02
#define EXPERT_AMP_COMMAND_BAND_UP 0x03
#define EXPERT_AMP_COMMAND_ANTENNA 0x04
#define EXPERT_AMP_COMMAND_L_MINUS 0x05
#define EXPERT_AMP_COMMAND_L_PLUS 0x06
#define EXPERT_AMP_COMMAND_C_MINUS 0x07
#define EXPERT_AMP_COMMAND_C_PLUS 0x08
#define EXPERT_AMP_COMMAND_TUNE 0x09
#define EXPERT_AMP_COMMAND_SWITCH_OFF 0x0A
#define EXPERT_AMP_COMMAND_POWER 0x0B
#define EXPERT_AMP_COMMAND_DISPLAY 0x0C
#define EXPERT_AMP_COMMAND_OPERATE 0x0D
#define EXPERT_AMP_COMMAND_CAT 0x0E
#define EXPERT_AMP_COMMAND_LEFT_ARROW 0x0F
#define EXPERT_AMP_COMMAND_RIGHT_ARROW 0x10
#define EXPERT_AMP_COMMAND_SET 0x11
#define EXPERT_AMP_COMMAND_BACKLIGHT_ON 0x82
#define EXPERT_AMP_COMMAND_BACKLIGHT_OFF 0x83
#define EXPERT_AMP_COMMAND_STATUS 0x90

// The following command is undocumented, but used by the Expert amplifier control application
// It outputs the LCD screen contents using an ASCII-like character set (needs conversion to ASCII)
#define EXPERT_AMP_COMMAND_SCREEN 0x80

#define EXPERT_ID_13K "13K"
#define EXPERT_ID_15K "15K"
#define EXPERT_ID_20K "20K"

#define EXPERT_STATE_STANDBY 'S'
#define EXPERT_STATE_OPERATE 'O'

#define EXPERT_PTT_RECEIVE 'R'
#define EXPERT_PTT_TRANSMIT 'T'

#define EXPERT_MEMORY_BANK_A 'A'
#define EXPERT_MEMORY_BANK_B 'B'
#define EXPERT_MEMORY_BANK_NONE 'x' // for 2K

#define EXPERT_BAND_160M 0
#define EXPERT_BAND_80M 1
#define EXPERT_BAND_60M 2
#define EXPERT_BAND_40M 3
#define EXPERT_BAND_30M 4
#define EXPERT_BAND_20M 5
#define EXPERT_BAND_17M 6
#define EXPERT_BAND_15M 7
#define EXPERT_BAND_12M 8
#define EXPERT_BAND_10M 9
#define EXPERT_BAND_6M 10
#define EXPERT_BAND_4M 11

#define EXPERT_TX_ANTENNA_STATUS_TUNABLE 't'
#define EXPERT_TX_ANTENNA_STATUS_ATU_BYPASSED 'b'
#define EXPERT_TX_ANTENNA_STATUS_ATU_ENABLED 'a'

#define EXPERT_POWER_LEVEL_LOW 'L'
#define EXPERT_POWER_LEVEL_MID 'M'
#define EXPERT_POWER_LEVEL_HIGH 'H'

typedef struct expert_fault_status_s
{
    char code;
    int status;
} expert_fault_status_code;

const expert_fault_status_code expert_warning_status_codes[] =
{
    {'N', AMP_STATUS_NONE},
    {'M', AMP_STATUS_WARNING_OTHER},
    {'S', AMP_STATUS_WARNING_SWR_HIGH},
    {'B', AMP_STATUS_WARNING_FREQUENCY},
    {'P', AMP_STATUS_WARNING_POWER_LIMIT},
    {'O', AMP_STATUS_WARNING_TEMPERATURE},
    {'Y', AMP_STATUS_WARNING_OTHER},
    {'W', AMP_STATUS_WARNING_TUNER_NO_INPUT},
    {'K', AMP_STATUS_WARNING_OTHER},
    {'R', AMP_STATUS_WARNING_OTHER},
    {'T', AMP_STATUS_WARNING_OTHER},
    {'C', AMP_STATUS_WARNING_OTHER},
    {0,   AMP_STATUS_NONE}
};

const expert_fault_status_code expert_alarm_status_codes[] =
{
    {'N', AMP_STATUS_NONE},
    {'S', AMP_STATUS_FAULT_SWR},
    {'D', AMP_STATUS_FAULT_INPUT_POWER},
    {'H', AMP_STATUS_FAULT_TEMPERATURE},
    {'C', AMP_STATUS_FAULT_OTHER},
    {0, AMP_STATUS_NONE}
};

typedef struct expert_fault_message_s
{
    char code;
    char *message;
} expert_fault_message;

const expert_fault_message expert_warning_messages[] =
{
    {'N', "No warnings"},
    {'M', "Amplifier alarm"},
    {'S', "Antenna SWR high"},
    {'B', "No valid band"},
    {'P', "Power limit exceeded"},
    {'O', "Overheating"},
    {'Y', "ATU not available"},
    {'W', "Tuning with no input power"},
    {'K', "ATU bypassed"},
    {'R', "Power switch held by remote"},
    {'T', "Combiner overheating"},
    {'C', "Combiner fault"},
    {0, NULL}
};

const expert_fault_message expert_alarm_messages [] =
{
    {'N', "No alarms"},
    {'S', "SWR exceeding limits"},
    {'D', "Input overdriving"},
    {'H', "Excess overheating"},
    {'C', "Combiner fault"},
    {0, NULL}
};

typedef struct expert_status_response_s
{
    char id[3];
    char sep1;
    char state;
    char sep2;
    char ptt;
    char sep3;
    char memory_bank;
    char sep4;
    char input;
    char sep5;
    char selected_band[2];
    char sep6;
    char tx_antenna;
    char tx_antenna_atu_status;
    char sep7;
    char rx_antenna;
    char rx_antenna_status;
    char sep8;
    char power_level;
    char sep9;
    char output_power[4];
    char sep10;
    char swr_atu[5];
    char sep11;
    char swr_ant[5];
    char sep12;
    char supply_voltage[4];
    char sep13;
    char supply_current[4];
    char sep14;
    char temperature_upper_heatsink[3];
    char sep15;
    char temperature_lower_heatsink[3];
    char sep16;
    char temperature_combiner[3];
    char sep17;
    char warning;
    char sep18;
    char alarm;
    char sep19;
    char checksum;
    char sep20;
} expert_status_response;

struct expert_priv_data
{
    char id[8];
    expert_status_response status_response;
    char fault[128];
    char warning[128];
};

static int expert_flushbuffer(AMP *amp)
{
    rig_debug(RIG_DEBUG_VERBOSE, "%s called\n", __func__);

    return rig_flush(AMPPORT(amp));
}

static int expert_transaction(AMP *amp, const unsigned char *cmd, unsigned char cmd_len,
                       unsigned char *response, int response_len)
{
    struct amp_state *rs;
    unsigned char cmdbuf[EXPERTBUFSZ];
    int err;
    int len = 0;
    int checksum = 0;

    rig_debug(RIG_DEBUG_VERBOSE, "%s called\n", __func__);

    if (!amp)
    {
        rig_debug(RIG_DEBUG_ERR, "%s: amp pointer null\n", __func__);
        return -RIG_EINVAL;
    }

    if (!cmd)
    {
        rig_debug(RIG_DEBUG_ERR, "%s: cmd empty\n", __func__);
        return -RIG_EINVAL;
    }

    expert_flushbuffer(amp);

    rs = &amp->state;

    // Synchronization bytes for commands
    cmdbuf[0] = cmdbuf[1] = cmdbuf[2] = 0x55;

    // Calculate checksum
    for (int i = 0; i < cmd_len; i++)
    {
        checksum += cmd[i];
        checksum = checksum % 256;
    }

    cmdbuf[3] = cmd_len;
    memcpy(&cmdbuf[4], cmd, cmd_len);
    cmdbuf[4 + cmd_len] = checksum;

    err = write_block(&rs->ampport, (unsigned char *) cmdbuf, 4 + cmd_len + 1);

    if (err != RIG_OK)
    {
        return err;
    }

    if (!response)
    {
        // No response expected
        return RIG_OK;
    }

    int bytes = 0;
    response[0] = 0;

    // Read the 4-byte header x55x55x55xXX where XX is the number of bytes in the response
    len = read_block_direct(&rs->ampport, response, 4);

    if (len < 0)
    {
        rig_debug(RIG_DEBUG_ERR, "%s: expected to read 4 bytes of response data, but reading failed with code %d\n",
                __func__, len);
        return len;
    }

    if (len != 4)
    {
        rig_debug(RIG_DEBUG_ERR, "%s: expected to read 4 bytes of response data, but got %d bytes\n",
                __func__, len);
        dump_hex(response, len);
        return -RIG_EPROTO;
    }

    for (int i = 0; i < 3; i++)
    {
        if (response[i] != 0xAA)
        {
            rig_debug(RIG_DEBUG_ERR, "%s: expected to read response header synchronization byte 0xAA, but got 0x%02X at index %d\n",
                    __func__, response[i], len);
            dump_hex(response, len);
            return -RIG_EPROTO;
        }
    }

    bytes = response[3];

    rig_debug(RIG_DEBUG_VERBOSE, "%s: len=%d, bytes=%02x\n", __func__, len, bytes);

    if (bytes > response_len)
    {
        rig_debug(RIG_DEBUG_ERR, "%s: response does not fit in buffer: response=%d buffer=%d\n",
                __func__, bytes, response_len);
        bytes = response_len;
    }

    len = read_block_direct(&rs->ampport, (unsigned  char *) response, bytes);
    if (len < 0)
    {
        rig_debug(RIG_DEBUG_ERR, "%s: expected to read %d bytes of response data, but reading failed with code %d\n",
                __func__, bytes, len);
        return len;
    }

    if (len != bytes)
    {
        rig_debug(RIG_DEBUG_ERR, "%s: expected to read %d bytes of response data, but got %d bytes\n",
                __func__, bytes, len);
        dump_hex(response, len);
        return -RIG_EPROTO;
    }

    return RIG_OK;
}

static int expert_read_status(AMP *amp, expert_status_response *status)
{
    unsigned char cmd = EXPERT_AMP_COMMAND_STATUS;
    unsigned char response[EXPERTBUFSZ];
    int result;

    // TODO: cache status for configurable time, e.g. 100ms?

    result = expert_transaction(amp, &cmd, 1, response, sizeof(response));
    if (result != RIG_OK)
    {
        rig_debug(RIG_DEBUG_ERR, "%s: error reading amplifier status, result=%d (%s)",
                __func__, result, rigerror(result));
        return result;
    }

    memcpy(status, response, sizeof(expert_status_response));

    return RIG_OK;
}

int expert_init(AMP *amp)
{
    rig_debug(RIG_DEBUG_VERBOSE, "%s called\n", __func__);

    if (!amp)
    {
        return -RIG_EINVAL;
    }

    amp->state.priv = (struct expert_priv_data *)
                      calloc(1, sizeof(struct expert_priv_data));

    if (!amp->state.priv)
    {
        return -RIG_ENOMEM;
    }

    amp->state.ampport.type.rig = RIG_PORT_SERIAL;

    return RIG_OK;
}

int expert_open(AMP *amp)
{
    unsigned char cmd = EXPERT_AMP_COMMAND_SCREEN;
    unsigned char response[EXPERTBUFSZ];
    struct expert_priv_data *priv = amp->state.priv;
    int result;

    rig_debug(RIG_DEBUG_VERBOSE, "%s called\n", __func__);

    expert_transaction(amp, &cmd, 1, response, sizeof(response));

    result = expert_read_status(amp, &priv->status_response);
    if (result == RIG_OK)
    {
        memcpy(priv->id, priv->status_response.id, 3);
        priv->id[3] = 0;
    }

    return RIG_OK;
}

int expert_close(AMP *amp)
{
    // TODO: what does command 0x81 do?
    unsigned char cmd = 0x81;
    unsigned char response[256];

    rig_debug(RIG_DEBUG_VERBOSE, "%s called\n", __func__);

    expert_transaction(amp, &cmd, 1, response, 4);

    if (amp->state.priv)
    {
        free(amp->state.priv);
    }

    amp->state.priv = NULL;

    return RIG_OK;
}

/*
 * Get Info
 * returns the model name string
 */
// cppcheck-suppress constParameterCallback
const char *expert_get_info(AMP *amp)
{
    const struct amp_caps *rc;

    rig_debug(RIG_DEBUG_VERBOSE, "%s called\n", __func__);

    if (!amp)
    {
        return NULL;
    }

    rc = amp->caps;

    return rc->model_name;
}

int expert_get_freq(AMP *amp, freq_t *freq)
{
    struct expert_priv_data *priv = amp->state.priv;
    expert_status_response *status_response = &priv->status_response;
    int result;
    int band;
    int nargs;

    rig_debug(RIG_DEBUG_VERBOSE, "%s called\n", __func__);

    if (!amp)
    {
        return -RIG_EINVAL;
    }

    result = expert_read_status(amp, status_response);
    if (result != RIG_OK)
    {
        return result;
    }

    nargs = sscanf(status_response->selected_band, "%02d", &band);

    if (nargs != 1)
    {
        rig_debug(RIG_DEBUG_ERR, "%s: error parsing amplifier band: response='%s'\n",
                __func__, status_response->selected_band);
        return -RIG_EPROTO;
    }

    switch (band)
    {
        case EXPERT_BAND_160M:
            *freq = 1800000UL;
            break;
        case EXPERT_BAND_80M:
            *freq = 3500000UL;
            break;
        case EXPERT_BAND_60M:
            *freq = 5351500UL;
            break;
        case EXPERT_BAND_40M:
            *freq = 7000000UL;
            break;
        case EXPERT_BAND_30M:
            *freq = 10100000UL;
            break;
        case EXPERT_BAND_20M:
            *freq = 14000000UL;
            break;
        case EXPERT_BAND_17M:
            *freq = 18068000UL;
            break;
        case EXPERT_BAND_15M:
            *freq = 21000000UL;
            break;
        case EXPERT_BAND_12M:
            *freq = 24890000UL;
            break;
        case EXPERT_BAND_10M:
            *freq = 28000000UL;
            break;
        case EXPERT_BAND_6M:
            *freq = 50000000UL;
            break;
        case EXPERT_BAND_4M:
            *freq = 70000000UL;
            break;
        default:
            rig_debug(RIG_DEBUG_VERBOSE, "%s: unknown band: %d\n",
                    __func__, band);
            return -RIG_EPROTO;
    }

    return RIG_OK;
}

int expert_get_level(AMP *amp, setting_t level, value_t *val)
{
    struct expert_priv_data *priv = amp->state.priv;
    expert_status_response *status_response = &priv->status_response;
    int result;
    int nargs;

    rig_debug(RIG_DEBUG_VERBOSE, "%s called\n", __func__);

    if (!amp)
    {
        return -RIG_EINVAL;
    }

    result = expert_read_status(amp, status_response);
    if (result != RIG_OK)
    {
        return result;
    }

    switch (level)
    {
    case AMP_LEVEL_SWR: {
        float swr;
        nargs = sscanf(status_response->swr_ant, "%02f", &swr);

        if (nargs != 1)
        {
            rig_debug(RIG_DEBUG_ERR, "%s: error parsing SWR: '%5s'\n",
                    __func__, status_response->swr_ant);
            return -RIG_EPROTO;
        }

        val->f = swr;
        break;
    }

    case AMP_LEVEL_SWR_TUNER: {
        float swr;
        nargs = sscanf(status_response->swr_atu, "%02f", &swr);

        if (nargs != 1)
        {
            rig_debug(RIG_DEBUG_ERR, "%s: error parsing SWR: '%5s'\n",
                    __func__, status_response->swr_atu);
            return -RIG_EPROTO;
        }

        val->f = swr;
        break;
    }

    case AMP_LEVEL_PWR_FWD:
    case AMP_LEVEL_PWR_PEAK: {
        int power;
        nargs = sscanf(status_response->output_power, "%04d", &power);

        if (nargs != 1)
        {
            rig_debug(RIG_DEBUG_ERR, "%s: error parsing power: '%4s'\n",
                    __func__, status_response->output_power);
            return -RIG_EPROTO;
        }

        val->i = power;
        break;
    }

    case AMP_LEVEL_RFPOWER: {
        char power_level = status_response->power_level;

        switch (power_level)
        {
            case EXPERT_POWER_LEVEL_LOW:
                val->f = 33.3f;
                break;
            case EXPERT_POWER_LEVEL_MID:
                val->f = 66.6f;
                break;
            case EXPERT_POWER_LEVEL_HIGH:
                val->f = 100.0f;
                break;
            default:
                rig_debug(RIG_DEBUG_ERR, "%s: error parsing power level: '%c'\n",
                        __func__, power_level);
                return -RIG_EPROTO;
        }

        break;
    }

    case AMP_LEVEL_VD_METER: {
        float voltage;
        nargs = sscanf(status_response->supply_voltage, "%04f", &voltage);

        if (nargs != 1)
        {
            rig_debug(RIG_DEBUG_ERR, "%s: error parsing voltage: '%4s'\n",
                    __func__, status_response->supply_voltage);
            return -RIG_EPROTO;
        }

        val->f = voltage;
        break;
    }
    case AMP_LEVEL_ID_METER: {
        float current;
        nargs = sscanf(status_response->supply_current, "%04f", &current);

        if (nargs != 1)
        {
            rig_debug(RIG_DEBUG_ERR, "%s: error parsing current: '%4s'\n",
                    __func__, status_response->supply_current);
            return -RIG_EPROTO;
        }

        val->f = current;
        break;
    }
    case AMP_LEVEL_TEMP_METER: {
        int temp;
        nargs = sscanf(status_response->temperature_upper_heatsink, "%03d", &temp);

        if (nargs != 1)
        {
            rig_debug(RIG_DEBUG_ERR, "%s: error parsing temperature: '%3s'\n",
                    __func__, status_response->temperature_upper_heatsink);
            return -RIG_EPROTO;
        }

        val->i = temp;
        break;
    }

    case AMP_LEVEL_FAULT: {
        char alarm = status_response->alarm;

        for (int i = 0; expert_alarm_messages[i].message != NULL; ++i)
        {
            if (expert_alarm_messages[i].code == alarm)
            {
                val->s = expert_alarm_messages[i].message;
                return RIG_OK;
            }
        }

        rig_debug(RIG_DEBUG_ERR, "%s: unknown alarm code: %c\n", __func__, alarm);

        SNPRINTF(priv->fault, sizeof(priv->fault), "Unknown alarm: %c", alarm);
        val->s = priv->fault;
        break;
    }
    case AMP_LEVEL_WARNING: {
        char warning = status_response->warning;

        for (int i = 0; expert_warning_messages[i].message != NULL; ++i)
        {
            if (expert_warning_messages[i].code == warning)
            {
                val->s = expert_warning_messages[i].message;
                return RIG_OK;
            }
        }

        rig_debug(RIG_DEBUG_ERR, "%s: unknown warning code: %c\n", __func__, warning);

        SNPRINTF(priv->warning, sizeof(priv->warning), "Unknown warning: %c", warning);

        val->s = priv->warning;
        break;
    }

    default:
        rig_debug(RIG_DEBUG_ERR, "%s: unknown level=%s\n", __func__,
                rig_strlevel(level));
    }

    return -RIG_EINVAL;
}

int expert_get_powerstat(AMP *amp, powerstat_t *status)
{
    struct expert_priv_data *priv = amp->state.priv;
    expert_status_response *status_response = &priv->status_response;
    int result;

    rig_debug(RIG_DEBUG_VERBOSE, "%s called\n", __func__);

    if (!amp)
    {
        return -RIG_EINVAL;
    }

    result = expert_read_status(amp, status_response);
    if (result != RIG_OK)
    {
        return result;
    }

    char state = status_response->state;

    switch (state)
    {
        case EXPERT_STATE_STANDBY:
            *status = RIG_POWER_STANDBY;
            break;
        case EXPERT_STATE_OPERATE:
            *status = RIG_POWER_OPERATE;
            break;
        default:
            rig_debug(RIG_DEBUG_ERR, "%s: error parsing power status: '%c'\n",
                    __func__, state);
            return -RIG_EPROTO;
    }

    return RIG_OK;
}

int expert_set_powerstat(AMP *amp, powerstat_t status)
{
    struct expert_priv_data *priv = amp->state.priv;
    expert_status_response *status_response = &priv->status_response;
    int result;
    int powered_on = 0;
    int operate = 0;
    int toggle_power = 0;
    int toggle_operate = 0;

    rig_debug(RIG_DEBUG_VERBOSE, "%s called\n", __func__);

    if (!amp)
    {
        return -RIG_EINVAL;
    }

    result = expert_read_status(amp, status_response);

    if (result == RIG_OK)
    {
        powered_on = 1;
        operate = (status_response->state == EXPERT_STATE_OPERATE) ? 1 : 0;
    }
    else
    {
        if (result == -RIG_ETIMEOUT || result == -RIG_EPROTO)
        {
            // Assume power is off if we receive a timeout or protocol error
            powered_on = 0;
        }
        else
        {
            return result;
        }
    }

    // Check first if we need to power ON or OFF
    switch (status)
    {
        case RIG_POWER_OFF:
            if (powered_on)
            {
                toggle_power = 1;
            }
            else
            {
                return RIG_OK;
            }
            break;
        case RIG_POWER_ON:
        case RIG_POWER_STANDBY:
        case RIG_POWER_OPERATE:
            if (!powered_on)
            {
                toggle_power = 1;
            }
            break;
        default:
            return -RIG_EINVAL;
    }

    if (toggle_power)
    {
        // TODO: toggle RTS pin for 2 seconds and wait for 2 seconds
        // TODO: read operate status again, retry 2 times if not successful
    }

    switch (status)
    {
        case RIG_POWER_OFF:
            break;
        case RIG_POWER_ON:
        case RIG_POWER_STANDBY:
            if (operate)
            {
                toggle_operate = 1;
            }
            break;
        case RIG_POWER_OPERATE:
            if (!operate)
            {
                toggle_operate = 1;
            }
            break;
        default:
            return -RIG_EINVAL;
    }

    if (toggle_operate)
    {
        unsigned char cmd = EXPERT_AMP_COMMAND_OPERATE;

        rig_debug(RIG_DEBUG_VERBOSE, "%s called\n", __func__);

        result = expert_transaction(amp, &cmd, 1, NULL, 0);
        if (result != RIG_OK)
        {
            return result;
        }

        // TODO: read operate status again, retry 2 times if not successful
    }

    return RIG_OK;
}

int expert_reset(AMP *amp, amp_reset_t reset)
{
    int result;

    rig_debug(RIG_DEBUG_VERBOSE, "%s called\n", __func__);

    result = expert_set_powerstat(amp, RIG_POWER_STANDBY);

    if (result != RIG_OK)
    {
        rig_debug(RIG_DEBUG_ERR, "%s: error setting RIG_POWER_STANDBY, result=%d (%s)\n", __func__,
                  result, strerror(result));
        return result;
    }

    return RIG_OK;
}


/*
 * Expert 1.3K-FA, 1.5K-FA, and 2K-FA
 */
const struct amp_caps expert_amp_caps =
{
    AMP_MODEL(AMP_MODEL_EXPERT_FA),
    .model_name = "1.3K-FA/1.5K-FA/2K-FA",
    .mfg_name = "Expert",
    .version = "20240115.0",
    .copyright = "LGPL",
    .status = RIG_STATUS_BETA,
    .amp_type = AMP_TYPE_OTHER,
    .port_type = RIG_PORT_SERIAL,
    .serial_rate_min = 9600,
    .serial_rate_max = 115200,
    .serial_data_bits = 8,
    .serial_stop_bits = 1,
    .serial_parity = RIG_PARITY_NONE,
    .serial_handshake = RIG_HANDSHAKE_NONE,
    .write_delay = 0,
    .post_write_delay = 0,
    .timeout = 2000,
    .retry = 2,

    .has_get_func = EXPERT_GET_FUNCS,
    .has_set_func = EXPERT_SET_FUNCS,
    .has_get_level = EXPERT_GET_LEVELS,
    .has_set_level = EXPERT_SET_LEVELS,
    .has_get_parm = EXPERT_GET_PARMS,
    .has_set_parm = EXPERT_SET_PARMS,

    .amp_ops = EXPERT_AMP_OPS,

    .amp_open = expert_open,
    .amp_init = expert_init,
    .amp_close = expert_close,
    .reset = expert_reset,
    .get_info = expert_get_info,
    .get_powerstat = expert_get_powerstat,
    .set_powerstat = expert_set_powerstat,
    .get_freq = expert_get_freq,
    //.get_input = expert_get_input,
    //.set_input = expert_set_input,
    //.get_ant = expert_get_ant,
    //.set_ant = expert_set_ant,
    //.get_func = expert_get_func,
    .get_level = expert_get_level,
    //.set_level = expert_set_level,
    //.set_parm = expert_set_parm,
    //.amp_op = expert_amp_op,

    // TODO: create Hamlib model for each amplifier model
    .range_list1 = {
        FRQ_RNG_HF(1, RIG_MODE_ALL, W(1), W(1500), RIG_VFO_ALL, EXPERT_ANTS),
        FRQ_RNG_6m(1, RIG_MODE_ALL, W(1), W(1500), RIG_VFO_ALL, EXPERT_ANTS),
        FRQ_RNG_60m(1, RIG_MODE_ALL, W(1), W(1500), RIG_VFO_ALL, EXPERT_ANTS),
        RIG_FRNG_END,
    },
    .range_list2 = {
        FRQ_RNG_HF(2, RIG_MODE_ALL, W(1), W(1500), RIG_VFO_ALL, EXPERT_ANTS),
        FRQ_RNG_6m(2, RIG_MODE_ALL, W(1), W(1500), RIG_VFO_ALL, EXPERT_ANTS),
        FRQ_RNG_60m(2, RIG_MODE_ALL, W(1), W(1500), RIG_VFO_ALL, EXPERT_ANTS),
        RIG_FRNG_END,
    },
};


/* ************************************
 *
 * API functions
 *
 * ************************************
 */


/*
 * Initialize backend
 */

DECLARE_INITAMP_BACKEND(expert)
{
    rig_debug(RIG_DEBUG_VERBOSE, "%s called\n", __func__);

    amp_register(&expert_amp_caps);

    return RIG_OK;
}
