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
#include <poll.h>
#include <pthread.h>
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

    /* A send to a connection that has gone must fail, not end the test with rigctld
     * still running */
    signal(SIGPIPE, SIG_IGN);
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
 * An emulated IC-7610 over TCP: answers the CI-V commands the backend sends while
 * opening the rig and reading its frequency, while it is answering. Stopped, it
 * closes its sockets, as a port that has gone away.
 */
#define CIV_RIG 0x98
#define CIV_CONTROLLER 0xe0
#define RIG_FREQ "14074000"

struct fake_rig
{
    int listener;
    int port;
    int answering;
    int stop;
    pthread_t thread;
};


static void fake_rig_reply(int sock, const unsigned char *payload, size_t len)
{
    unsigned char frame[32] = { 0xfe, 0xfe, CIV_CONTROLLER, CIV_RIG };

    memcpy(frame + 4, payload, len);
    frame[4 + len] = 0xfd;
    send(sock, frame, 5 + len, 0);
}


static void fake_rig_answer(int sock, const unsigned char *command, size_t len)
{
    /* 14.074000 MHz as five BCD bytes, least significant first */
    static const unsigned char freq[5] = { 0x00, 0x40, 0x07, 0x14, 0x00 };
    unsigned char reply[16];
    unsigned char cmd = command[0];
    unsigned char sub = len > 1 ? command[1] : 0;

    switch (cmd)
    {
    case 0x03:
        reply[0] = cmd;
        memcpy(reply + 1, freq, sizeof(freq));
        fake_rig_reply(sock, reply, 1 + sizeof(freq));
        break;

    case 0x25:
        reply[0] = cmd;
        reply[1] = sub;
        memcpy(reply + 2, freq, sizeof(freq));
        fake_rig_reply(sock, reply, 2 + sizeof(freq));
        break;

    case 0x07:
        reply[0] = cmd;
        reply[1] = sub;
        reply[2] = 0;
        fake_rig_reply(sock, reply, 3);
        break;

    case 0x14:
        reply[0] = cmd;
        reply[1] = sub;
        reply[2] = 0;
        reply[3] = 0;
        fake_rig_reply(sock, reply, 4);
        break;

    default:
        reply[0] = 0xfb;
        fake_rig_reply(sock, reply, 1);
        break;
    }
}


static void *fake_rig_run(void *arg)
{
    struct fake_rig *rig = arg;
    unsigned char pending[1024];
    size_t have = 0;
    int client = -1;

    while (!__atomic_load_n(&rig->stop, __ATOMIC_RELAXED))
    {
        struct pollfd fds[2] = { { rig->listener, POLLIN, 0 }, { client, POLLIN, 0 } };
        unsigned char *start;
        ssize_t got;

        if (poll(fds, client >= 0 ? 2 : 1, 50) <= 0)
        {
            continue;
        }

        if (fds[0].revents & POLLIN)
        {
            if (client >= 0)
            {
                close(client);
            }

            client = accept(rig->listener, NULL, NULL);
            have = 0;
            continue;
        }

        if (client < 0 || !(fds[1].revents & (POLLIN | POLLHUP)))
        {
            continue;
        }

        got = recv(client, pending + have, sizeof(pending) - have, 0);

        if (got <= 0)
        {
            close(client);
            client = -1;
            continue;
        }

        have += (size_t)got;

        /* Whole frames addressed to the rig: fe fe <rig> <controller> ... fd */
        while ((start = memchr(pending, 0xfe, have)) != NULL)
        {
            size_t skip = (size_t)(start - pending);
            unsigned char *end = memchr(start, 0xfd, have - skip);

            if (end == NULL)
            {
                break;
            }

            if (end - start >= 5 && start[1] == 0xfe && start[2] == CIV_RIG
                    && start[3] == CIV_CONTROLLER
                    && __atomic_load_n(&rig->answering, __ATOMIC_RELAXED))
            {
                fake_rig_answer(client, start + 4, (size_t)(end - start) - 4);
            }

            have -= (size_t)(end - pending) + 1;
            memmove(pending, end + 1, have);
        }

        if (have == sizeof(pending))
        {
            have = 0;
        }
    }

    if (client >= 0)
    {
        close(client);
    }

    return NULL;
}


static int fake_rig_start(struct fake_rig *rig, int answering)
{
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);

    memset(rig, 0, sizeof(*rig));
    rig->answering = answering;
    rig->listener = socket(AF_INET, SOCK_STREAM, 0);

    if (rig->listener < 0)
    {
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(rig->listener, (struct sockaddr *)&addr, sizeof(addr)) != 0
            || listen(rig->listener, 4) != 0
            || getsockname(rig->listener, (struct sockaddr *)&addr, &len) != 0
            || pthread_create(&rig->thread, NULL, fake_rig_run, rig) != 0)
    {
        close(rig->listener);
        rig->listener = -1;
        return -1;
    }

    rig->port = ntohs(addr.sin_port);
    return 0;
}


static void fake_rig_set_answering(struct fake_rig *rig, int answering)
{
    __atomic_store_n(&rig->answering, answering, __ATOMIC_RELAXED);
}


static void fake_rig_stop(struct fake_rig *rig)
{
    if (rig->listener >= 0)
    {
        __atomic_store_n(&rig->stop, 1, __ATOMIC_RELAXED);
        pthread_join(rig->thread, NULL);
        close(rig->listener);
        rig->listener = -1;
    }
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


/*
 * A rig that stops answering must not cost a client its connection: a timeout is
 * returned like any other error, and the same connection is served once the rig is
 * back. When the port goes away every read is answered with an error, while a
 * command answered from Hamlib's own state does not need the rig.
 */
void test_client_stays_connected_when_rig_does_not_answer(void)
{
    struct fake_rig rig;
    struct rigctld_proc proc;
    char rig_path[32];
    char reply[128];
    const char *args[] = { "-m", "3078", "-r", rig_path, "-C", "cache_timeout=0", NULL };
    int sock, i;

    TEST_ASSERT(fake_rig_start(&rig, 1) == 0);
    snprintf(rig_path, sizeof(rig_path), "127.0.0.1:%d", rig.port);

    if (start_rigctld(&proc, args) != 0 || (sock = open_first_client(&proc)) < 0)
    {
        TEST_CHECK_(0, "could not start rigctld");
        stop_rigctld(&proc);
        fake_rig_stop(&rig);
        return;
    }


    send_command(sock, "f\n");
    expect_reply(sock, "read with the rig answering", RIG_FREQ);

    /* The cable is pulled, for longer than rigctld once took to give up reopening
     * the rig and drop the client */
    fake_rig_set_answering(&rig, 0);
    send_command(sock, "f\n");
    expect_reply(sock, "read with the rig silent", "RPRT -5");
    sleep(6);
    fake_rig_set_answering(&rig, 1);
    sleep(1);
    send_command(sock, "f\n");
    expect_reply(sock, "read after the rig came back", RIG_FREQ);

    /* The port goes away and the rig cannot be reopened */
    fake_rig_stop(&rig);

    for (i = 0; i < 5; i++)
    {
        send_command(sock, "f\n");
        read_reply(sock, reply, sizeof(reply));
        TEST_CHECK_(strncmp(reply, "RPRT -", 6) == 0,
                    "read %d with the port gone: got '%s'", i + 1, reply);
    }

    send_command(sock, "\\chk_vfo\n");
    expect_reply(sock, "chk_vfo with the port gone", "0");

    close(sock);
    stop_rigctld(&proc);
}


TEST_LIST =
{
    { "chk_vfo_answers_each_clients_own_mode", test_chk_vfo_answers_each_clients_own_mode },
    { "client_stays_connected_when_rig_does_not_answer", test_client_stays_connected_when_rig_does_not_answer },
    { NULL, NULL }
};

#endif  /* _WIN32 */
