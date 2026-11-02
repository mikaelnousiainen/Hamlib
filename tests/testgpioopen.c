/*
 * Opens a GPIO line through a directory standing in for /sys/class/gpio.
 *
 * Exporting a line makes its direction and value files at once, but root's
 * alone: udev gives them to the group that may use the line a moment later.
 * A program in that group that opens the line as soon as it has exported it
 * must wait for its files to become writable. When they never do, the open
 * fails and the line it exported is unexported again.
 *
 * gpio.c is built into this test with GPIO_SYSFS_PATH naming the directory,
 * whose export and unexport are plain files: what the open writes to them is
 * read back. The files' permissions stand in for udev's, so the test is
 * skipped as root, whom they do not stop.
 */

#if defined(_WIN32) || defined(WIN32)

int main(void)
{
    return 77;
}

#else

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "hamlib/rig.h"
#include "hamlib/port.h"
#include "gpio.h"

#define LINE "591"
#define LINE_DIR GPIO_SYSFS_PATH "/gpio" LINE

/* udev's delay, and the longest the open may take when the files never become
 * writable: it waits about 3 s, and far longer than that only if it hangs, the
 * margin being for a loaded machine, whose short sleeps may take far longer */
#define PERMISSIONS_DELAY_US 400000
#define GIVE_UP_LIMIT_MS 30000

static int failed;

static void fail(const char *what)
{
    fprintf(stderr, "FAIL: %s\n", what);
    failed = 1;
}

static void write_file(const char *path, const char *content, mode_t mode)
{
    FILE *file;

    chmod(path, 0600);
    file = fopen(path, "w");

    if (file == NULL)
    {
        perror(path);
        exit(1);
    }

    fputs(content, file);
    fclose(file);
    chmod(path, mode);
}

static int file_is(const char *path, const char *content)
{
    char buffer[64] = { 0 };
    FILE *file = fopen(path, "r");
    size_t length;

    if (file == NULL)
    {
        return 0;
    }

    length = fread(buffer, 1, sizeof(buffer) - 1, file);
    fclose(file);
    return length == strlen(content) && strcmp(buffer, content) == 0;
}

/* A sysfs GPIO directory, the line's files made by the export as the kernel
 * makes them: writable by root alone */
static void make_sysfs(void)
{
    mkdir(GPIO_SYSFS_PATH, 0700);
    mkdir(LINE_DIR, 0700);
    write_file(GPIO_SYSFS_PATH "/export", "", 0600);
    write_file(GPIO_SYSFS_PATH "/unexport", "", 0600);
    write_file(LINE_DIR "/direction", "in\n", 0444);
    write_file(LINE_DIR "/value", "0\n", 0444);
}

static void remove_sysfs(void)
{
    unlink(LINE_DIR "/direction");
    unlink(LINE_DIR "/value");
    rmdir(LINE_DIR);
    unlink(GPIO_SYSFS_PATH "/export");
    unlink(GPIO_SYSFS_PATH "/unexport");
    rmdir(GPIO_SYSFS_PATH);
}

/* Gives the line's files to the group a moment after the open starts, as udev
 * does, in a process of its own */
static pid_t give_permissions_later(void)
{
    pid_t pid = fork();

    if (pid == 0)
    {
        usleep(PERMISSIONS_DELAY_US);
        chmod(LINE_DIR "/direction", 0664);
        chmod(LINE_DIR "/value", 0664);
        _exit(0);
    }

    return pid;
}

static long elapsed_ms(const struct timeval *from)
{
    struct timeval now;

    gettimeofday(&now, NULL);
    return (now.tv_sec - from->tv_sec) * 1000L + (now.tv_usec - from->tv_usec) / 1000L;
}

static void test_open_waits_for_permissions(void)
{
    hamlib_port_t port;
    pid_t udev;
    int fd;

    make_sysfs();
    memset(&port, 0, sizeof(port));
    strcpy(port.pathname, LINE);
    udev = give_permissions_later();

    fd = gpio_open(&port, 1, 1);
    waitpid(udev, NULL, 0);

    if (fd < 0)
    {
        fail("the open did not wait for the line's files to become writable");
        remove_sysfs();
        return;
    }

    if (!file_is(GPIO_SYSFS_PATH "/export", LINE "\n"))
    {
        fail("the line was not exported");
    }

    if (!file_is(LINE_DIR "/direction", "out\n"))
    {
        fail("the line's direction was not set to out");
    }

    if (gpio_ptt_set(&port, RIG_PTT_ON) != RIG_OK
            || !file_is(LINE_DIR "/value", "1\n"))
    {
        fail("keying did not write 1 to the line's value");
    }

    if (!file_is(GPIO_SYSFS_PATH "/unexport", ""))
    {
        fail("the line was unexported while open");
    }

    gpio_close(&port);

    if (!file_is(GPIO_SYSFS_PATH "/unexport", LINE "\n"))
    {
        fail("closing did not unexport the line");
    }

    remove_sysfs();
}

static void test_open_gives_up_and_unexports(void)
{
    hamlib_port_t port;
    struct timeval start;
    int fd;

    make_sysfs();
    memset(&port, 0, sizeof(port));
    strcpy(port.pathname, LINE);
    gettimeofday(&start, NULL);

    fd = gpio_open(&port, 1, 1);

    if (fd >= 0)
    {
        fail("the open succeeded though the line's files never became writable");
        close(fd);
    }

    if (elapsed_ms(&start) > GIVE_UP_LIMIT_MS)
    {
        fail("the open waited too long for the line's files");
    }

    if (!file_is(GPIO_SYSFS_PATH "/unexport", LINE "\n"))
    {
        fail("the failed open left the line it exported exported");
    }

    remove_sysfs();
}

int main(void)
{
    if (geteuid() == 0)
    {
        fprintf(stderr, "skipped: the files' permissions do not stop root\n");
        return 77;
    }

    rig_set_debug(RIG_DEBUG_NONE);
    remove_sysfs();
    test_open_waits_for_permissions();
    test_open_gives_up_and_unexports();
    return failed;
}

#endif
