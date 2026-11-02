/*
 *  Hamlib Interface - gpio support
 *  Copyright (c) 2016 by Jeroen Vreeken
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

#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>

#include "gpio.h"
#include "hamlib/port.h"

/* The sysfs GPIO interface, which a test builds this file over a directory of its own for */
#ifndef GPIO_SYSFS_PATH
#define GPIO_SYSFS_PATH "/sys/class/gpio"
#endif

/* How long the files of a line just exported are waited for, and how often
 * they are tried: the kernel makes them root's, and udev gives them to the
 * group that may use the line a moment later */
#define GPIO_FILE_WAIT_MS 3000
#define GPIO_FILE_POLL_MS 50


/* Opens a file of an exported line, waiting while it is missing or not yet
 * open to us */
static int gpio_open_line_file(const char *pathname, int flags)
{
    int waited_ms = 0;

    for (;;)
    {
        int fd = open(pathname, flags);

        if (fd >= 0 || (errno != EACCES && errno != ENOENT)
                || waited_ms >= GPIO_FILE_WAIT_MS)
        {
            return fd;
        }

        hl_usleep(GPIO_FILE_POLL_MS * 1000);
        waited_ms += GPIO_FILE_POLL_MS;
    }
}


static int gpio_unexport(const hamlib_port_t *port)
{
    char pathname[HAMLIB_FILPATHLEN * 2];
    FILE *fexp;

    SNPRINTF(pathname, HAMLIB_FILPATHLEN, "%s/unexport", GPIO_SYSFS_PATH);
    fexp = fopen(pathname, "w");

    if (!fexp)
    {
        rig_debug(RIG_DEBUG_ERR,
                  "Unexport GPIO%s (using %s): %s\n",
                  port->pathname,
                  pathname,
                  strerror(errno));
        return -RIG_EIO;
    }

    fprintf(fexp, "%s\n", port->pathname);
    fclose(fexp);
    return RIG_OK;
}


int gpio_open(hamlib_port_t *port, int output, int on_value)
{
    char pathname[HAMLIB_FILPATHLEN * 2];
    FILE *fexp;
    int fd, exported;
    const char *dir;

    port->parm.gpio.on_value = on_value;

    SNPRINTF(pathname, HAMLIB_FILPATHLEN, "%s/export", GPIO_SYSFS_PATH);
    fexp = fopen(pathname, "w");

    if (!fexp)
    {
        rig_debug(RIG_DEBUG_ERR,
                  "Export GPIO%s (using %s): %s\n",
                  port->pathname,
                  pathname,
                  strerror(errno));
        return -RIG_EIO;
    }

    /* A line exported already, by another program, refuses the export */
    exported = fprintf(fexp, "%s\n", port->pathname) > 0;
    exported = fclose(fexp) == 0 && exported;

    SNPRINTF(pathname,
             sizeof(pathname),
             "%s/gpio%s/direction",
             GPIO_SYSFS_PATH, port->pathname);
    fd = gpio_open_line_file(pathname, O_WRONLY);

    if (fd < 0)
    {
        rig_debug(RIG_DEBUG_ERR,
                  "GPIO%s direction (using %s): %s\n",
                  port->pathname,
                  pathname,
                  strerror(errno));

        if (exported)
        {
            gpio_unexport(port);
        }

        return -RIG_EIO;
    }

    dir = output ? "out\n" : "in\n";
    rig_debug(RIG_DEBUG_VERBOSE, "Setting direction of GPIO%s to %s",
              port->pathname, dir);

    if (write(fd, dir, strlen(dir)) < 0)
    {
        rig_debug(RIG_DEBUG_ERR,
                  "GPIO%s setting direction (using %s): %s\n",
                  port->pathname,
                  pathname,
                  strerror(errno));
        close(fd);

        if (exported)
        {
            gpio_unexport(port);
        }

        return -RIG_EIO;
    }

    close(fd);

    SNPRINTF(pathname,
             sizeof(pathname),
             "%s/gpio%s/value",
             GPIO_SYSFS_PATH, port->pathname);
    fd = gpio_open_line_file(pathname, O_RDWR);

    if (fd < 0)
    {
        rig_debug(RIG_DEBUG_ERR,
                  "GPIO%s opening value file %s: %s\n",
                  port->pathname,
                  pathname,
                  strerror(errno));

        if (exported)
        {
            gpio_unexport(port);
        }

        return -RIG_EIO;
    }

    port->fd = fd;
    return fd;
}


int gpio_close(hamlib_port_t *port)
{
    int retval = close(port->fd);

    if (gpio_unexport(port) != RIG_OK)
    {
        return -RIG_EIO;
    }

    return retval;
}


int gpio_ptt_set(hamlib_port_t *port, ptt_t pttx)
{
    char *val;
    port->parm.gpio.value = pttx != RIG_PTT_OFF;

    if ((port->parm.gpio.value && port->parm.gpio.on_value)
            || (!port->parm.gpio.value && !port->parm.gpio.on_value))
    {
        val = "1\n";
    }
    else
    {
        val = "0\n";
    }

    if (write(port->fd, val, strlen(val)) <= 0)
    {
        return -RIG_EIO;
    }

    return RIG_OK;
}


int gpio_ptt_get(hamlib_port_t *port, ptt_t *pttx)
{
    if (port->parm.gpio.value)
    {
        *pttx = RIG_PTT_ON;
    }
    else
    {
        *pttx = RIG_PTT_OFF;
    }

    return RIG_OK;
}

int gpio_dcd_get(hamlib_port_t *port, dcd_t *dcdx)
{
    char val;
    int port_value;

    lseek(port->fd, 0, SEEK_SET);

    if (read(port->fd, &val, sizeof(val)) <= 0)
    {
        return -RIG_EIO;
    }

    rig_debug(RIG_DEBUG_VERBOSE, "DCD GPIO pin value: %c\n", val);

    port_value = val - '0';

    if (port_value == port->parm.gpio.on_value)
    {
        *dcdx = RIG_DCD_ON;
    }
    else
    {
        *dcdx = RIG_DCD_OFF;
    }

    return RIG_OK;
}
