//include <stdio.h>
//#include <stdlib.h>
//#include <string.h>
//#include <errno.h>
//#include <unistd.h>
#include <hamlib/rig.h>
//#include <sys/socket.h>
#ifdef HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#ifdef HAVE_ARPA_INET_H
#include <arpa/inet.h>
#endif

#ifndef MULTICAST_H
#define MULTICAST_H
#ifdef HAVE_ARPA_INET_H
#warn HAVE_ARPA_INET
    struct ip_mreq mreq; // = {0};
    struct sockaddr_in dest_addr; // = {0};
#else
#warn DO NOT HAVE ARPA_INET_H
#endif
};
#endif

struct multicast_vfo
{
    char *name;
    double freq;
    char *mode;
    int width;
    int widthLower;
    int widthUpper;
    unsigned char rx; // true if in rx mode
    unsigned char tx; // true in in tx mode
};

struct multicast_broadcast
{
    char *ID;
    struct multicast_vfo **vfo;
};

// returns # of bytes sent
extern HAMLIB_EXPORT (int) multicast_init(RIG *rig, char *addr, int port);
extern HAMLIB_EXPORT (int) multicast_send(RIG *rig, const char *msg, int msglen);
#endif //MULTICAST_H
