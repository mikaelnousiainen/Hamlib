/*
 * Hamlib Icom VFO read across a standby
 * Copyright (c) 2026 by Hamlib Team
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

/*
 * An IC-7610 switched off at its front panel keeps CI-V alive and answers reads
 * with NG. One refused VFO read must not stop the VFO being read: the read after
 * the rig is switched on again must reach the rig.
 */

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "hamlib/rig.h"
#include "hamlib/port.h"
#include "hamlib/rig_state.h"
#include "icom.h"
#include "misc.h"

extern struct rig_caps ic7610_caps;

#define READS 3

static const unsigned char request_vfo[] =
{
    0xfe, 0xfe, 0x98, 0xe0, 0x07, 0xd2, 0xfd
};
static const unsigned char response_main[] =
{
    0xfe, 0xfe, 0xe0, 0x98, 0x07, 0xd2, 0x00, 0xfd
};
static const unsigned char response_ng[] =
{
    0xfe, 0xfe, 0xe0, 0x98, 0xfa, 0xfd
};

struct peer
{
    int fd;
    int answered;
};

static int close_test_socket(int fd)
{
#ifdef _WIN32
    return closesocket(fd);
#else
    return close(fd);
#endif
}

static int open_test_connection(int sockets[2])
{
#ifdef _WIN32
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    SOCKET client;
    SOCKET accepted;
    struct sockaddr_in address;
    int address_length = sizeof(address);

    if (listener == INVALID_SOCKET) { return -1; }

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(listener, (struct sockaddr *) &address, sizeof(address)) != 0
            || listen(listener, 1) != 0
            || getsockname(listener, (struct sockaddr *) &address,
                           &address_length) != 0)
    {
        closesocket(listener);
        return -1;
    }

    client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    if (client == INVALID_SOCKET
            || connect(client, (struct sockaddr *) &address, sizeof(address)) != 0)
    {
        closesocket(listener);
        return -1;
    }

    accepted = accept(listener, NULL, NULL);
    closesocket(listener);

    if (accepted == INVALID_SOCKET)
    {
        closesocket(client);
        return -1;
    }

    sockets[0] = (int) client;
    sockets[1] = (int) accepted;
    return 0;
#else
    return socketpair(AF_UNIX, SOCK_STREAM, 0, sockets);
#endif
}

/* Answers the VFO reads: on, in standby, on again */
static void *run_peer(void *arg)
{
    struct peer *peer = arg;
    unsigned char request[32];
    int i;

    for (i = 0; i < READS; i++)
    {
        size_t length = 0;
        const unsigned char *response = i == 1 ? response_ng : response_main;
        size_t response_length = i == 1 ? sizeof(response_ng) : sizeof(response_main);

        while (length < sizeof(request))
        {
            if (recv(peer->fd, (char *) request + length, 1, 0) != 1)
            {
                return NULL;
            }

            if (request[length++] == 0xfd)
            {
                break;
            }
        }

        if (length != sizeof(request_vfo)
                || memcmp(request, request_vfo, sizeof(request_vfo)) != 0)
        {
            fprintf(stderr, "read %d: unexpected request\n", i + 1);
            return NULL;
        }

        if (send(peer->fd, (const char *) response, response_length, 0)
                != (int) response_length)
        {
            return NULL;
        }

        peer->answered++;
    }

    return NULL;
}

static int expect_read(RIG *rig, const char *what, vfo_t expected)
{
    vfo_t vfo = RIG_VFO_NONE;
    int retval = rig_get_vfo(rig, &vfo);

    if (retval != RIG_OK || vfo != expected)
    {
        fprintf(stderr, "%s: expected %s, got %s: %s\n", what, rig_strvfo(expected),
                rig_strvfo(vfo), rigerror(retval));
        return 1;
    }

    return 0;
}

int main(void)
{
    int sockets[2];
    int failures = 0;
    pthread_t thread;
    struct peer peer = { 0 };
    struct icom_priv_data *priv;
    RIG *rig;

#ifdef _WIN32
    WSADATA wsa_data;

    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0)
    {
        return 1;
    }

#endif

    rig_set_debug(RIG_DEBUG_NONE);
    rig_register(&ic7610_caps);

    if (open_test_connection(sockets) != 0)
    {
        fprintf(stderr, "test socket setup failed\n");
        return 1;
    }

    peer.fd = sockets[1];

    if (pthread_create(&thread, NULL, run_peer, &peer) != 0)
    {
        return 1;
    }

    rig = rig_init(RIG_MODEL_IC7610);

    if (rig == NULL)
    {
        return 1;
    }

    STATE(rig)->comm_state = 1;
    RIGPORT(rig)->fd = sockets[0];
    RIGPORT(rig)->type.rig = RIG_PORT_NETWORK;
    RIGPORT(rig)->retry = 0;
    RIGPORT(rig)->timeout = 100;
    priv = STATE(rig)->priv;
    priv->serial_USB_echo_off = 1;
    rig_set_cache_timeout_ms(rig, HAMLIB_CACHE_ALL, 0);

    failures += expect_read(rig, "the rig on", RIG_VFO_MAIN);
    /* A refused read is taken as VFO A, for that call */
    failures += expect_read(rig, "the rig in standby", RIG_VFO_A);
    failures += expect_read(rig, "the rig on again", RIG_VFO_MAIN);

    if (ic7610_caps.get_vfo == NULL)
    {
        fprintf(stderr, "a refused read changed the capabilities\n");
        failures++;
    }

    STATE(rig)->comm_state = 0;
    RIGPORT(rig)->fd = -1;
    close_test_socket(sockets[0]);
    rig_cleanup(rig);
    pthread_join(thread, NULL);
    close_test_socket(sockets[1]);

    if (peer.answered != READS)
    {
        fprintf(stderr, "the rig was asked %d times, expected %d\n", peer.answered,
                READS);
        failures++;
    }

    return failures != 0;
}
