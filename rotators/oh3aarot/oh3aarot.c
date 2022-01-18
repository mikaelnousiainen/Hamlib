/*
 *  Hamlib OH3AA rotator controller backend - main file
 *  Copyright (c) 2019-2020 by Mikael Nousiainen
 *  Based on Ether6 backend by Stephane Fillod and Jonny Röker
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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>  /* String function definitions */
#include <stdlib.h>

#include <hamlib/rotator.h>
#include "serial.h"
#include "register.h"
#include "idx_builtin.h"

#include "oh3aarot.h"

#define CMD_MAX 32
#define BUF_MAX 128

#define OH3AAROT_PROTOCOL_RESPONSE_OK "OK"

#define OH3AAROT_LEVELS ROT_LEVEL_SPEED

#define OH3AAROT_ROT_STATUS (ROT_STATUS_MOVING | ROT_STATUS_MOVING_AZ | ROT_STATUS_MOVING_LEFT | ROT_STATUS_MOVING_RIGHT | \
        ROT_STATUS_LIMIT_LEFT | ROT_STATUS_LIMIT_RIGHT | ROT_STATUS_OVERLAP_LEFT | ROT_STATUS_OVERLAP_RIGHT)

#define OH3AAROT_BACKEND_INFO "OH3AA IP network-based rotator controller"

struct oh3aarot_rot_priv_data
{
    char controller_info[BUF_MAX];
    char backend_info[BUF_MAX * 2];
};

static int oh3aarot_transaction(ROT *rot, char *cmd, char *resp)
{
    int ret;

    ret = write_block(&rot->state.rotport, (unsigned char *) cmd, strlen(cmd));
    rig_debug(RIG_DEBUG_VERBOSE, "function %s(1): ret=%d command=%s\n", __func__, ret, cmd);
    if (ret != 0) {
        return ret;
    }

    ret = read_string(&rot->state.rotport, (unsigned char *) resp, BUF_MAX, "\n", sizeof("\n"), 0, 1);
    rig_debug(RIG_DEBUG_VERBOSE, "function %s(2): ret=%d response=%s\n", __func__, ret, resp);
    if (ret < 0) {
        return ret;
    }

    if (memcmp(resp, OH3AAROT_PROTOCOL_RESPONSE_OK, strlen(OH3AAROT_PROTOCOL_RESPONSE_OK)) != 0) {
        rig_debug(RIG_DEBUG_VERBOSE, "function %s(2a): invalid response=%s\n", __func__, resp);
        return -RIG_EPROTO;
    }

    resp[strcspn(resp, "\r\n")] = 0;

    return ret;
}

static int oh3aarot_rot_init(ROT *rot)
{
    struct oh3aarot_rot_priv_data *priv;

    rig_debug(RIG_DEBUG_VERBOSE, "%s called\n", __func__);

    rot->state.priv = (struct oh3aarot_rot_priv_data *)
            malloc(sizeof(struct oh3aarot_rot_priv_data));

    if (!rot->state.priv) {
        return -RIG_ENOMEM;
    }

    priv = rot->state.priv;

    priv->controller_info[0] = '\0';
    strncpy(priv->backend_info, OH3AAROT_BACKEND_INFO, BUF_MAX);

    return RIG_OK;
}

static int oh3aarot_rot_cleanup(ROT *rot)
{
    rig_debug(RIG_DEBUG_VERBOSE, "%s called\n", __func__);

    if (rot->state.priv)
    {
        free(rot->state.priv);
    }

    rot->state.priv = NULL;

    return RIG_OK;
}

static int oh3aarot_rot_open(ROT *rot)
{
    int ret, matches;
    float min_az, max_az;
    struct rot_state *rs = &rot->state;
    struct oh3aarot_rot_priv_data *priv =
            (struct oh3aarot_rot_priv_data *) rot->state.priv;

    char cmd[CMD_MAX];
    char resp[BUF_MAX];

    rig_debug(RIG_DEBUG_VERBOSE, "%s called\n", __func__);

    sprintf(cmd, "INFO\n");

    ret = oh3aarot_transaction(rot, cmd, resp);
    if (ret <= 0) {
        rig_debug(RIG_DEBUG_ERR, "%s: invalid response to INFO command\n", __func__);
        return (ret < 0) ? ret : -RIG_EPROTO;
    }

    rig_debug(RIG_DEBUG_VERBOSE, "%s: rotator controller info: %s\n", __func__, resp);

    strncpy(priv->controller_info, resp, BUF_MAX);
    strncpy(priv->backend_info, OH3AAROT_BACKEND_INFO, BUF_MAX);
    strncat(priv->backend_info, ": ", BUF_MAX);
    strncat(priv->backend_info, resp, BUF_MAX);

    sprintf(cmd, "AZLIMITS\n");

    ret = oh3aarot_transaction(rot, cmd, resp);
    if (ret <= 0) {
        return (ret < 0) ? ret : -RIG_EPROTO;
    }

    matches = sscanf(resp, "OK AZLIMITS MIN=%f MAX=%f", &min_az, &max_az);
    if (matches != 2) {
        rig_debug(RIG_DEBUG_ERR, "%s: invalid response to AZLIMITS command\n", __func__);
        return -RIG_EPROTO;
    }

    rs->min_az = min_az;
    rs->max_az = max_az;
    rs->min_el = 0;
    rs->max_el = 0;

    rig_debug(RIG_DEBUG_VERBOSE, "Azimuth limits: min=%f max=%f\n", rs->min_az, rs->max_az);

    return RIG_OK;
}

static int oh3aarot_rot_close(ROT *rot)
{
    rig_debug(RIG_DEBUG_VERBOSE, "%s called\n", __func__);

    write_block(&rot->state.rotport, (unsigned char *) "\n", 1);

    return RIG_OK;
}

static int oh3aarot_command(ROT *rot, char *cmd)
{
    int ret;
    char buf[BUF_MAX];

    rig_debug(RIG_DEBUG_VERBOSE, "%s called: cmd=%s\n", __func__, cmd);

    ret = oh3aarot_transaction(rot, cmd, buf);
    if (ret <= 0) {
        return (ret < 0) ? ret : -RIG_EPROTO;
    }

    return RIG_OK;
}

static int oh3aarot_rot_set_position(ROT *rot, azimuth_t az, elevation_t el)
{
    char cmd[CMD_MAX];

    rig_debug(RIG_DEBUG_VERBOSE, "%s called: %f %f\n", __func__, az, el);

    sprintf(cmd, "AZ %f\n", az);

    return oh3aarot_command(rot, cmd);
}

static int oh3aarot_rot_get_position(ROT *rot, azimuth_t *az, elevation_t *el)
{
    int ret, matches;
    char cmd[CMD_MAX];
    char buf[BUF_MAX];

    rig_debug(RIG_DEBUG_VERBOSE, "%s called\n", __func__);

    sprintf(cmd, "AZ?\n");

    ret = oh3aarot_transaction(rot, cmd, buf);
    if (ret <= 0) {
        return (ret < 0) ? ret : -RIG_EPROTO;
    }

    matches = sscanf(buf, "OK AZ %f", az);
    *el = 0;

    rig_debug(RIG_DEBUG_VERBOSE, "%s: az=%f\n", __func__, *az);

    if (matches != 1) {
        return -RIG_EPROTO;
    }

    return RIG_OK;
}

static int oh3aarot_rot_stop(ROT *rot)
{
    char cmd[CMD_MAX];

    rig_debug(RIG_DEBUG_VERBOSE, "%s called\n", __func__);

    sprintf(cmd, "STOP\n");

    return oh3aarot_command(rot, cmd);
}


static int oh3aarot_rot_park(ROT *rot)
{
    char cmd[CMD_MAX];

    rig_debug(RIG_DEBUG_VERBOSE, "%s called\n", __func__);

    sprintf(cmd, "PARK\n");

    return oh3aarot_command(rot, cmd);
}

static int oh3aarot_rot_reset(ROT *rot, rot_reset_t reset)
{
    char cmd[CMD_MAX];

    rig_debug(RIG_DEBUG_VERBOSE, "%s called\n", __func__);

    sprintf(cmd, "RESET\n");

    return oh3aarot_command(rot, cmd);
}

static int oh3aarot_rot_get_level(ROT *rot, setting_t level, value_t *val)
{
    char cmd[CMD_MAX];
    char buf[BUF_MAX];
    int ret, matches;

    rig_debug(RIG_DEBUG_VERBOSE, "%s called: %s\n", __func__, rot_strlevel(level));

    switch (level) {
        case ROT_LEVEL_SPEED: {
            int speed;
            sprintf(cmd, "SPEED?\n");

            ret = oh3aarot_transaction(rot, cmd, buf);
            if (ret <= 0) {
                return (ret < 0) ? ret : -RIG_EPROTO;
            }

            matches = sscanf(buf, "OK SPEED %d", &speed);

            rig_debug(RIG_DEBUG_VERBOSE, "%s: speed=%d\n", __func__, speed);

            if (matches != 1) {
                return -RIG_EPROTO;
            }

            val->i = speed;
            break;
        }
        default:
            return -RIG_ENAVAIL;
    }

    return RIG_OK;
}


static int oh3aarot_rot_set_level(ROT *rot, setting_t level, value_t val)
{
    char cmd[CMD_MAX];
    int ret;

    rig_debug(RIG_DEBUG_VERBOSE, "%s called: %s\n", __func__, rot_strlevel(level));

    switch (level) {
        case ROT_LEVEL_SPEED: {
            int speed = val.i;
            if (speed < 1 || speed > 100) {
                rig_debug(RIG_DEBUG_ERR, "%s: invalid speed %d\n", __func__, speed);
                return -RIG_EINVAL;
            }

            sprintf(cmd, "SPEED %d\n", speed);

            ret = oh3aarot_command(rot, cmd);
            if (ret < 0) {
                return ret;
            }
            break;
        }
        default:
            return -RIG_ENAVAIL;
    }

    return RIG_OK;
}

static int oh3aarot_rot_move(ROT *rot, int direction, int speed)
{
    int ret;
    char cmd[CMD_MAX];
    char *dir_param;

    rig_debug(RIG_DEBUG_VERBOSE, "%s called: direction=%d speed=%d\n", __func__, direction, speed);

    switch (direction) {
        case ROT_MOVE_CW:
            dir_param = "CW";
            break;
        case ROT_MOVE_CCW:
            dir_param = "CCW";
            break;
        default:
            rig_debug(RIG_DEBUG_ERR, "%s: invalid direction %d\n", __func__, direction);
            return -RIG_EINVAL;
    }

    if (speed != ROT_SPEED_NOCHANGE) {
        value_t speed_val = { .i = speed };

        ret = oh3aarot_rot_set_level(rot, ROT_LEVEL_SPEED, speed_val);
        if (ret != RIG_OK)
        {
            return ret;
        }
    }

    sprintf(cmd, "MOVE %s\n", dir_param);

    return oh3aarot_command(rot, cmd);
}

static const char *oh3aarot_rot_get_info(ROT *rot)
{
    struct oh3aarot_rot_priv_data *priv =
            (struct oh3aarot_rot_priv_data *) rot->state.priv;

    rig_debug(RIG_DEBUG_VERBOSE, "%s called\n", __func__);

    return priv->backend_info;
}

static int oh3aarot_rot_find_flag(char *flag_str)
{
    if (strcmp(flag_str, "CW") == 0) {
        return ROT_STATUS_MOVING | ROT_STATUS_MOVING_AZ | ROT_STATUS_MOVING_RIGHT;
    } else if (strcmp(flag_str, "CCW") == 0) {
        return ROT_STATUS_MOVING | ROT_STATUS_MOVING_AZ | ROT_STATUS_MOVING_LEFT;
    } else if (strcmp(flag_str, "T1") == 0) {
        return ROT_STATUS_OVERLAP_LEFT;
    } else if (strcmp(flag_str, "T2") == 0) {
        return ROT_STATUS_OVERLAP_RIGHT;
    } else if (strcmp(flag_str, "L1") == 0) {
        return ROT_STATUS_LIMIT_LEFT;
    } else if (strcmp(flag_str, "L2") == 0) {
        return ROT_STATUS_LIMIT_RIGHT;
    }

    return 0;
}

static int oh3aarot_rot_get_status(ROT *rot, rot_status_t *status)
{
    int ret;
    char cmd[CMD_MAX];
    char buf[BUF_MAX];
    char *flags_prefix = "FLAGS=";
    char *flags_str;
    char *flags_saveptr = NULL;
    int flags = 0;

    rig_debug(RIG_DEBUG_VERBOSE, "%s called\n", __func__);

    sprintf(cmd, "STATE\n");

    ret = oh3aarot_transaction(rot, cmd, buf);
    if (ret <= 0) {
        return (ret < 0) ? ret : -RIG_EPROTO;
    }

    flags_str = strstr(buf, flags_prefix);
    if (flags_str == NULL) {
        rig_debug(RIG_DEBUG_ERR, "%s: no status flags found in state response\n", __func__);
        return -RIG_EPROTO;
    }

    flags_str += strlen(flags_prefix);

    char *flag_str = strtok_r(flags_str, ",", &flags_saveptr);
    if (flag_str == NULL) {
        flags |= oh3aarot_rot_find_flag(flags_str);
    } else {
        while (flag_str != NULL) {
            flags |= oh3aarot_rot_find_flag(flag_str);
            flag_str = strtok_r(NULL, ",", &flags_saveptr);
        }
    }

    rig_debug(RIG_DEBUG_VERBOSE, "%s: flags_str=%s flags=0x%08x\n",
            __func__, flags_str, flags);

    *status = flags;

    return RIG_OK;
}

const struct rot_caps oh3aarot_rot_caps = {
    ROT_MODEL(ROT_MODEL_OH3AAROT1),
    .model_name =     "OH3AArot 1",
    .mfg_name =       "OH3AA",
    .version =        "20201206",
    .copyright =      "LGPL",
    .status =         RIG_STATUS_BETA,
    .rot_type =       ROT_FLAG_AZIMUTH,
    .port_type =      RIG_PORT_NETWORK,
    .timeout = 5000,
    .retry =   3,

    .min_az =    -90,
    .max_az =    450,
    .min_el =    0,
    .max_el =    0,

    .priv =  NULL,  /* priv */

    .has_status = OH3AAROT_ROT_STATUS,

    .has_get_level =  OH3AAROT_LEVELS,
    .has_set_level =  ROT_LEVEL_SET(OH3AAROT_LEVELS),

    .level_gran =      { [ROT_LVL_SPEED] = { .min = { .i = 1 }, .max = { .i = 100 }, .step = { .i = 1 } } },

    .rot_init     =  oh3aarot_rot_init,
    .rot_cleanup  =  oh3aarot_rot_cleanup,
    .rot_open     =  oh3aarot_rot_open,
    .rot_close    =  oh3aarot_rot_close,

    .set_position =  oh3aarot_rot_set_position,
    .get_position =  oh3aarot_rot_get_position,
    .park         =  oh3aarot_rot_park,
    .stop         =  oh3aarot_rot_stop,
    .reset        =  oh3aarot_rot_reset,
    .move         =  oh3aarot_rot_move,
    .get_level    =  oh3aarot_rot_get_level,
    .set_level    =  oh3aarot_rot_set_level,

    .get_info     =  oh3aarot_rot_get_info,
    .get_status   =  oh3aarot_rot_get_status,
};

DECLARE_INITROT_BACKEND(oh3aarot)
{
    rig_debug(RIG_DEBUG_VERBOSE, "%s called\n", __func__);

    rot_register(&oh3aarot_rot_caps);

    return RIG_OK;
}
