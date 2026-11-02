/*
 *  Hamlib Interface - rigctld sessions as its clients see them
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

/*
 * Each test starts a real rigctld and talks to it over sockets, as applications
 * do: the raw protocol, not the network backend, which would keep state of its
 * own between the commands under test.
 */

#ifdef HAVE_CONFIG_H
#  include "hamlib/config.h"
#endif

#include "acutest.h"
#include "test_debug.h"

#ifdef _WIN32

/* rigctld is started as a child process with fork()/execv() */
static void test_requires_posix_host(void)
{
    TEST_MSG("rigctld session tests need fork(); skipped on this host");
    TEST_CHECK(1);
}

TEST_LIST =
{
    { "requires_posix_host", test_requires_posix_host },
    { NULL, NULL }
};

#else

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>

#define RIGCTLD_PATH "../tests/rigctld"
#define STARTUP_TIMEOUT_MS 10000
#define REPLY_TIMEOUT_S 20
#define MAX_RIGCTLD_ARGS 16

struct rigctld_proc
{
    pid_t pid;
    int port;
    char log_path[64];
};


static int pick_free_port(void)
{
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    int port = -1;

    if (sock < 0)
    {
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) == 0
            && getsockname(sock, (struct sockaddr *)&addr, &len) == 0)
    {
        port = ntohs(addr.sin_port);
    }

    close(sock);
    return port;
}


static int connect_to(int port)
{
    struct sockaddr_in addr;
    int sock = socket(AF_INET, SOCK_STREAM, 0);

    if (sock < 0)
    {
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)port);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0)
    {
        close(sock);
        return -1;
    }

    return sock;
}


static int rigctld_alive(struct rigctld_proc *proc)
{
    int status;

    return proc->pid > 0 && waitpid(proc->pid, &status, WNOHANG) == 0;
}


/* args: the options after -t, ended by NULL */
static int start_rigctld(struct rigctld_proc *proc, const char *const *args)
{
    char port_str[16];
    char *argv[MAX_RIGCTLD_ARGS];
    int argc = 0;

    memset(proc, 0, sizeof(*proc));

    if (access(RIGCTLD_PATH, X_OK) != 0)
    {
        TEST_MSG("rigctld is not built (expected at %s)", RIGCTLD_PATH);
        return -1;
    }

    proc->port = pick_free_port();

    if (proc->port < 0)
    {
        return -1;
    }

    snprintf(port_str, sizeof(port_str), "%d", proc->port);
    snprintf(proc->log_path, sizeof(proc->log_path), "rigctld-sessions-%d.log",
             proc->port);

    argv[argc++] = (char *)"rigctld";
    argv[argc++] = (char *)"-t";
    argv[argc++] = port_str;

    while (*args != NULL && argc < MAX_RIGCTLD_ARGS - 1)
    {
        argv[argc++] = (char *) * args++;
    }

    argv[argc] = NULL;
    proc->pid = fork();

    if (proc->pid < 0)
    {
        return -1;
    }

    if (proc->pid == 0)
    {
        if (freopen(proc->log_path, "w", stderr) == NULL
                || dup2(STDERR_FILENO, STDOUT_FILENO) < 0)
        {
            _exit(126);
        }

        execv(RIGCTLD_PATH, argv);
        _exit(127);
    }

    return 0;
}


/*
 * Connects once rigctld accepts connections. This connection is the client's own:
 * a throwaway one to see whether rigctld is up would be torn down in rigctld's
 * client thread while the next connections are being accepted.
 */
static int open_first_client(struct rigctld_proc *proc)
{
    struct timeval tv = { REPLY_TIMEOUT_S, 0 };
    int waited;

    for (waited = 0; waited < STARTUP_TIMEOUT_MS; waited += 20)
    {
        int sock;

        if (!rigctld_alive(proc))
        {
            TEST_MSG("rigctld exited during startup");
            proc->pid = 0;
            return -1;
        }

        sock = connect_to(proc->port);

        if (sock >= 0)
        {
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            return sock;
        }

        usleep(20 * 1000);
    }

    TEST_MSG("rigctld did not accept connections within %d ms",
             STARTUP_TIMEOUT_MS);
    return -1;
}


static void stop_rigctld(struct rigctld_proc *proc)
{
    if (proc->pid > 0)
    {
        kill(proc->pid, SIGTERM);
        waitpid(proc->pid, NULL, 0);
        proc->pid = 0;
    }

    /* A test artefact: left behind, it fails distcleancheck */
    if (proc->log_path[0] != '\0')
    {
        unlink(proc->log_path);
        proc->log_path[0] = '\0';
    }
}


static int open_client(const struct rigctld_proc *proc)
{
    struct timeval tv = { REPLY_TIMEOUT_S, 0 };
    int sock = connect_to(proc->port);

    if (sock >= 0)
    {
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    return sock;
}


static void send_command(int sock, const char *command)
{
    TEST_CHECK_(send(sock, command, strlen(command), 0)
                == (ssize_t)strlen(command), "sending %s", command);
}


/* Reads one line of a reply, without its line end; empty if none came */
static const char *read_reply(int sock, char *reply, size_t size)
{
    size_t got = 0;

    while (got < size - 1)
    {
        char c;

        if (recv(sock, &c, 1, 0) != 1)
        {
            TEST_MSG("no reply: %s", strerror(errno));
            break;
        }

        if (c == '\n')
        {
            break;
        }

        if (c != '\r')
        {
            reply[got++] = c;
        }
    }

    reply[got] = '\0';
    return reply;
}


static void expect_reply(int sock, const char *what, const char *expected)
{
    char reply[128];

    read_reply(sock, reply, sizeof(reply));
    TEST_CHECK_(strcmp(reply, expected) == 0, "%s: got '%s', expected '%s'",
                what, reply, expected);
}


/*
 * rigctld keeps each client's VFO mode with its connection, and the rig's state,
 * shared by every client, holds that of the connection whose command runs. A
 * command waiting for the daemon's lock must not have its connection's mode
 * overwritten by another client's, or \chk_vfo answers that client's mode, and a
 * program told 1 names a VFO before the arguments of its commands.
 */
void test_chk_vfo_answers_each_clients_own_mode(void)
{
    static const char *const args[] = { "-m", "1", NULL };
    struct rigctld_proc proc;
    char reply[128];
    int vfo_mode, holder, plain;

    if (start_rigctld(&proc, args) != 0
            || (vfo_mode = open_first_client(&proc)) < 0)
    {
        TEST_CHECK_(0, "could not start rigctld");
        stop_rigctld(&proc);
        return;
    }

    holder = open_client(&proc);
    plain = open_client(&proc);
    TEST_ASSERT(vfo_mode >= 0 && holder >= 0 && plain >= 0);

    send_command(vfo_mode, "\\set_vfo_opt 1\n");
    expect_reply(vfo_mode, "set_vfo_opt", "RPRT 0");
    send_command(plain, "\\chk_vfo\n");
    expect_reply(plain, "plain client alone", "0");
    send_command(vfo_mode, "\\chk_vfo\n");
    expect_reply(vfo_mode, "VFO mode client alone", "1");

    /* The plain client asks first, then the VFO mode client sends a command, both
     * waiting behind the third client's pause */
    send_command(holder, "\\pause 1\n");
    usleep(300 * 1000);
    send_command(plain, "\\chk_vfo\n");
    usleep(300 * 1000);
    send_command(vfo_mode, "f VFOA\n");
    expect_reply(plain, "plain client waiting", "0");
    read_reply(vfo_mode, reply, sizeof(reply));
    read_reply(holder, reply, sizeof(reply));

    /* And the other way round */
    send_command(holder, "\\pause 1\n");
    usleep(300 * 1000);
    send_command(vfo_mode, "\\chk_vfo\n");
    usleep(300 * 1000);
    send_command(plain, "f\n");
    expect_reply(vfo_mode, "VFO mode client waiting", "1");
    read_reply(plain, reply, sizeof(reply));
    read_reply(holder, reply, sizeof(reply));

    close(vfo_mode);
    close(holder);
    close(plain);
    stop_rigctld(&proc);
}


TEST_LIST =
{
    { "chk_vfo_answers_each_clients_own_mode", test_chk_vfo_answers_each_clients_own_mode },
    { NULL, NULL }
};

#endif  /* _WIN32 */
