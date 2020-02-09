/*
 *  Hamlib OH3AA rotator controller backend - main file
 *  Copyright (c) 2019 by Mikael Nousiainen
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

#include <stdlib.h>
#include <string.h>  /* String function definitions */
#include <unistd.h>  /* UNIX standard function definitions */
#include <math.h>
#include <sys/time.h>
#include <time.h>

#include <hamlib/rotator.h>
#include "serial.h"
#include "misc.h"
#include "register.h"

#include "oh3aarot.h"

#define CMD_MAX 32
#define BUF_MAX 128

#define OH3AAROT_PROTOCOL_RESPONSE_OK "OK"

static int oh3aarot_transaction(ROT *rot, char *cmd, char *resp)
{
    int ret;

    ret = write_block(&rot->state.rotport, cmd, strlen(cmd));
    rig_debug(RIG_DEBUG_VERBOSE, "function %s(1): ret=%d command=%s\n", __FUNCTION__, ret, cmd);
    if (ret != 0) {
        return ret;
    }

    ret = read_string(&rot->state.rotport, resp, BUF_MAX, "\n", sizeof("\n"));
    rig_debug(RIG_DEBUG_VERBOSE, "function %s(2): ret=%d response=%s\n", __FUNCTION__, ret, resp);
    if (ret < 0) {
        return ret;
    }

    if (memcmp(resp, OH3AAROT_PROTOCOL_RESPONSE_OK, strlen(OH3AAROT_PROTOCOL_RESPONSE_OK))) {
        rig_debug(RIG_DEBUG_VERBOSE, "function %s(2a): invalid response=%s\n", __FUNCTION__, resp);
        return -RIG_EPROTO;
    }

    return ret;
}

static int oh3aarot_rot_open(ROT *rot)
{
    int ret, matches;
    float min_az, max_az;
    struct rot_state *rs = &rot->state;

    char cmd[CMD_MAX];
    char resp[BUF_MAX];

    rig_debug(RIG_DEBUG_VERBOSE, "%s called\n", __FUNCTION__);

    sprintf(cmd, "INFO\n");

    ret = oh3aarot_transaction(rot, cmd, resp);
    if (ret <= 0) {
        return (ret < 0) ? ret : -RIG_EPROTO;
    }

    sprintf(cmd, "AZLIMITS\n");

    ret = oh3aarot_transaction(rot, cmd, resp);
    if (ret <= 0) {
        return (ret < 0) ? ret : -RIG_EPROTO;
    }

    matches = sscanf(resp, "OK AZLIMITS MIN=%f MAX=%f", &min_az, &max_az);
    if (matches != 2) {
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
    rig_debug(RIG_DEBUG_VERBOSE, "%s called\n", __FUNCTION__);

    write_block(&rot->state.rotport, "\n", 1);

    return RIG_OK;
}

static int oh3aarot_command(ROT *rot, char *cmd)
{
    int ret;
    char buf[BUF_MAX];

    rig_debug(RIG_DEBUG_VERBOSE, "%s called: cmd=%s\n", __FUNCTION__, cmd);

    ret = oh3aarot_transaction(rot, cmd, buf);
    if (ret <= 0) {
        return (ret < 0) ? ret : -RIG_EPROTO;
    }

    return RIG_OK;
}

static int oh3aarot_rot_set_position(ROT *rot, azimuth_t az, elevation_t el)
{
    char cmd[CMD_MAX];

    rig_debug(RIG_DEBUG_VERBOSE, "%s called: %f %f\n", __FUNCTION__, az, el);

    sprintf(cmd, "AZ %f\n", az);

    return oh3aarot_command(rot, cmd);
}

static int oh3aarot_rot_get_position(ROT *rot, azimuth_t *az, elevation_t *el)
{
    int ret, matches;
    char cmd[CMD_MAX];
    char buf[BUF_MAX];

    rig_debug(RIG_DEBUG_VERBOSE, "%s called\n", __FUNCTION__);

    sprintf(cmd, "AZ?\n");

    ret = oh3aarot_transaction(rot, cmd, buf);
    if (ret <= 0) {
        return (ret < 0) ? ret : -RIG_EPROTO;
    }

    matches = sscanf(buf, "OK AZ %f", az);
    *el = 0;

    rig_debug(RIG_DEBUG_VERBOSE, "az=%f\n", *az);

    if (matches != 1) {
        return -RIG_EPROTO;
    }

    return RIG_OK;
}

static int oh3aarot_rot_stop(ROT *rot)
{
    char cmd[CMD_MAX];

    rig_debug(RIG_DEBUG_VERBOSE, "%s called: %f %f\n", __FUNCTION__);

    sprintf(cmd, "STOP\n");

    return oh3aarot_command(rot, cmd);
}


static int oh3aarot_rot_park(ROT *rot)
{
    char cmd[CMD_MAX];

    rig_debug(RIG_DEBUG_VERBOSE, "%s called: %f %f\n", __FUNCTION__);

    sprintf(cmd, "PARK\n");

    return oh3aarot_command(rot, cmd);
}

static int oh3aarot_rot_reset(ROT *rot, rot_reset_t reset)
{
    char cmd[CMD_MAX];

    rig_debug(RIG_DEBUG_VERBOSE, "%s called: %f %f\n", __FUNCTION__);

    sprintf(cmd, "RESET\n");

    return oh3aarot_command(rot, cmd);
}

static int oh3aarot_rot_move(ROT *rot, int direction, int speed)
{
    int ret;
    char cmd[CMD_MAX];
    char *dir_param;

    rig_debug(RIG_DEBUG_VERBOSE, "%s called: %d %d\n", __FUNCTION__, direction, speed);

    switch (direction) {
        case ROT_MOVE_CW:
            dir_param = "CW";
            break;
        case ROT_MOVE_CCW:
            dir_param = "CCW";
            break;
        default:
            return -RIG_EINVAL;
    }

    sprintf(cmd, "SPEED %d\n", speed);

    ret = oh3aarot_command(rot, cmd);
    if (ret < 0) {
        return ret;
    }

    sprintf(cmd, "MOVE %s\n", dir_param);

    return oh3aarot_command(rot, cmd);
}


static const char *oh3aarot_rot_get_info(ROT *rot)
{
    rig_debug(RIG_DEBUG_VERBOSE, "%s called\n", __func__);

    return "OH3AA IP network-based rotator controller";
}

const struct rot_caps oh3aarot_rot_caps = {
    .rot_model =      ROT_MODEL_OH3AAROT,
    .model_name =     "OH3AArot 1",
    .mfg_name =       "OH3AA",
    .version =        "0.1",
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

    .rot_open     =  oh3aarot_rot_open,
    .rot_close    =  oh3aarot_rot_close,

    .set_position =  oh3aarot_rot_set_position,
    .get_position =  oh3aarot_rot_get_position,
    .park         =  oh3aarot_rot_park,
    .stop         =  oh3aarot_rot_stop,
    .reset        =  oh3aarot_rot_reset,
    .move         =  oh3aarot_rot_move,

    .get_info     =  oh3aarot_rot_get_info,
};

DECLARE_INITROT_BACKEND(oh3aarot)
{
    rig_debug(RIG_DEBUG_VERBOSE, "%s called\n", __FUNCTION__);

    rot_register(&oh3aarot_rot_caps);

    return RIG_OK;
}
