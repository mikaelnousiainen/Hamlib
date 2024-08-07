// can run this using rigctl/rigctld and socat pty devices
// gcc -o simyaesu simyaesu.c
#define _XOPEN_SOURCE 700
// since we are POSIX here we need this
#if 0
struct ip_mreq
{
    int dummy;
};
#endif

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include "../include/hamlib/rig.h"

#define BUFSIZE 256

float freqA = 14074000;
float freqB = 14074500;
char tx_vfo = '1';
char rx_vfo = '1';
char modeA = '0';
char modeB = '0';
int keyspd = 20;
int bandselect = 5;
int  width = 21;
int narrow = 0;
int vd = 0;
int sm0 = 0;
int sm1 = 0;

// ID 0310 == 310, Must drop leading zero
typedef enum nc_rigid_e
{
    NC_RIGID_NONE            = 0,
    NC_RIGID_FT450           = 241,
    NC_RIGID_FT450D          = 244,
    NC_RIGID_FT950           = 310,
    NC_RIGID_FT891           = 135,
    NC_RIGID_FT991           = 135,
    NC_RIGID_FT2000          = 251,
    NC_RIGID_FT2000D         = 252,
    NC_RIGID_FTDX1200        = 583,
    NC_RIGID_FTDX9000D       = 101,
    NC_RIGID_FTDX9000Contest = 102,
    NC_RIGID_FTDX9000MP      = 103,
    NC_RIGID_FTDX5000        = 362,
    NC_RIGID_FTDX3000        = 460,
    NC_RIGID_FTDX101D        = 681,
    NC_RIGID_FTDX101MP       = 682
} nc_rigid_t;

int
getmyline(int fd, char *buf)
{
    char c;
    int i = 0;
    memset(buf, 0, BUFSIZE);

    while (read(fd, &c, 1) > 0)
    {
        buf[i++] = c;

        if (c == ';') { return strlen(buf); }
    }

    if (strlen(buf) == 0) { hl_usleep(10 * 1000); }

    return strlen(buf);
}

#if defined(WIN32) || defined(_WIN32)
int openPort(char *comport) // doesn't matter for using pts devices
{
    int fd;
    fd = open(comport, O_RDWR);

    if (fd < 0)
    {
        perror(comport);
    }

    return fd;
}

#else
int openPort(char *comport) // doesn't matter for using pts devices
{
    int fd = posix_openpt(O_RDWR);
    char *name = ptsname(fd);

    if (name == NULL)
    {
        perror("pstname");
        return -1;
    }

    printf("name=%s\n", name);

    if (fd == -1 || grantpt(fd) == -1 || unlockpt(fd) == -1)
    {
        perror("posix_openpt");
        return -1;
    }

    return fd;
}
#endif



int main(int argc, char *argv[])
{
    char buf[256];
    char *pbuf;
    int n;
    int fd = openPort(argv[1]);

    while (1)
    {
        if (getmyline(fd, buf))
        {
            printf("Cmd:%s\n", buf);
        }
        else { continue; }

        if (strcmp(buf, ";") == 0)
        {
            pbuf = "?;";
            n = write(fd, pbuf, strlen(pbuf));
            printf("n=%d\n", n);
        }
        else if (strcmp(buf, "RM5;") == 0)
        {
            printf("%s\n", buf);
            hl_usleep(50 * 1000);
            pbuf = "RM5100000;";
            n = write(fd, pbuf, strlen(pbuf));
            printf("n=%d\n", n);

            if (n <= 0) { perror("RM5"); }
        }
        else if (strcmp(buf, "MR118;") == 0)
        {
            pbuf = "?;";
            n = write(fd, pbuf, strlen(pbuf));

            if (n <= 0) { perror("MR118"); }
        }

        else if (strcmp(buf, "AN0;") == 0)
        {
            printf("%s\n", buf);
            hl_usleep(50 * 1000);
            pbuf = "AN030;";
            n = write(fd, pbuf, strlen(pbuf));
            printf("n=%d\n", n);

            if (n <= 0) { perror("AN"); }
        }
        else if (strcmp(buf, "IF;") == 0)
        {
            printf("%s\n", buf);
            hl_usleep(50 * 1000);
            pbuf = "IF059014200000+000000700000;";
            n = write(fd, pbuf, strlen(pbuf));
            printf("n=%d\n", n);

            if (n <= 0) { perror("IF"); }
        }
        else if (strcmp(buf, "FA;") == 0)
        {
            SNPRINTF(buf, sizeof(buf), "FA%09.0f;", freqA);
            n = write(fd, buf, strlen(buf));
        }
        else if (strncmp(buf, "FA", 2) == 0)
        {
            sscanf(buf, "FA%f", &freqA);
        }
        else if (strcmp(buf, "FB;") == 0)
        {
            SNPRINTF(buf, sizeof(buf), "FB%09.0f;", freqB);
            n = write(fd, buf, strlen(buf));
        }
        else if (strncmp(buf, "FB", 2) == 0)
        {
            sscanf(buf, "FB%f", &freqB);
        }
        else if (strcmp(buf, "ID;") == 0)
        {
            printf("%s\n", buf);
            hl_usleep(50 * 1000);
            int id = NC_RIGID_FTDX3000;
            SNPRINTF(buf, sizeof(buf), "ID%03d;", id);
            n = write(fd, buf, strlen(buf));
            printf("n=%d\n", n);

            if (n <= 0) { perror("ID"); }
        }
        else if (strcmp(buf, "PS;") == 0)
        {
            SNPRINTF(buf, sizeof(buf), "PS1;");
            n = write(fd, buf, strlen(buf));
        }
        else if (strcmp(buf, "AI;") == 0)
        {
            printf("%s\n", buf);
            hl_usleep(50 * 1000);
            SNPRINTF(buf, sizeof(buf), "AI0;");
            n = write(fd, buf, strlen(buf));
            printf("n=%d\n", n);

            if (n <= 0) { perror("ID"); }
        }
        else if (strcmp(buf, "AI0;") == 0)
        {
            hl_usleep(50 * 1000);
        }
        else if (strcmp(buf, "FT;") == 0)
        {
            hl_usleep(50 * 1000);
            SNPRINTF(buf, sizeof(buf), "FT%c;", tx_vfo);
            printf(" FT response#1=%s, tx_vfo=%c\n", buf, tx_vfo);
            n = write(fd, buf, strlen(buf));
            printf(" FT response#2=%s\n", buf);

            if (n < 0) { perror("FT"); }
        }
        else if (strncmp(buf, "FT", 2) == 0)
        {
            tx_vfo = buf[2];

            if (tx_vfo == '3') { tx_vfo = '1'; }
            else if (tx_vfo == '2') { tx_vfo = '0'; }
            else { perror("Expected 2 or 3"); }
        }
        else if (strcmp(buf, "MD0;") == 0)
        {
            hl_usleep(50 * 1000);
            SNPRINTF(buf, sizeof(buf), "MD0%c;", modeA);
            n = write(fd, buf, strlen(buf));

            if (n < 0) { perror("MD0;"); }
        }
        else if (strncmp(buf, "MD0", 3) == 0)
        {
            modeA = buf[3];
        }
        else if (strcmp(buf, "MD1;") == 0)
        {
            hl_usleep(50 * 1000);
            SNPRINTF(buf, sizeof(buf), "MD1%c;", modeB);
            n = write(fd, buf, strlen(buf));

            if (n < 0) { perror("MD0;"); }
        }
        else if (strncmp(buf, "MD1", 3) == 0)
        {
            modeB = buf[3];
        }



#if 0
        else if (strncmp(buf, "AI", 2) == 0)
        {
            if (strcmp(buf, "AI;"))
            {
                printf("%s\n", buf);
                hl_usleep(50 * 1000);
                n = fprintf(fp, "%s", "AI0;");
                printf("n=%d\n", n);

                if (n <= 0) { perror("AI"); }
            }
        }

#endif
        else if (strcmp(buf, "VS;") == 0)
        {
            printf("%s\n", buf);
            hl_usleep(50 * 1000);
            pbuf = "VS0;";
            n = write(fd, pbuf, strlen(pbuf));
            printf("n=%d\n", n);

            if (n < 0) { perror("VS"); }
        }
        else if (strcmp(buf, "EX032;") == 0)
        {
            static int ant = 0;
            ant = (ant + 1) % 3;
            printf("%s\n", buf);
            hl_usleep(50 * 1000);
            SNPRINTF(buf, sizeof(buf), "EX032%1d;", ant);
            n = write(fd, buf, strlen(buf));
            printf("n=%d\n", n);

            if (n < 0) { perror("EX032"); }
        }
        else if (strncmp(buf, "KS;", 3) == 0)
        {
            sprintf(buf, "KS%d;", keyspd);
            n = write(fd, buf, strlen(buf));
        }
        else if (strncmp(buf, "KS", 2) == 0)
        {
            sscanf(buf, "KS%03d", &keyspd);
        }
        else if (strncmp(buf, "BS;", 3) == 0) // cannot query BS
        {
            sprintf(buf, "BS%02d;", bandselect);
            n = write(fd, buf, strlen(buf));
        }
        else if (strncmp(buf, "SH0;", 4) == 0)
        {
            sprintf(buf, "SH0%02d;", width);
            n = write(fd, buf, strlen(buf));
        }
        else if (strncmp(buf, "SH0", 3) == 0)
        {
            sscanf(buf, "SH0%02d", &width);
        }
        else if (strncmp(buf, "NA0;", 4) == 0)
        {
            sprintf(buf, "NA0%d;", narrow);
            n = write(fd, buf, strlen(buf));
        }
        else if (strncmp(buf, "NA0", 3) == 0)
        {
            sscanf(buf, "NA0%d", &narrow);
        }
        else if (strncmp(buf, "VD;", 3) == 0)
        {
            sprintf(buf, "VD%d;", vd);
            n = write(fd, buf, strlen(buf));
        }
        else if (strncmp(buf, "VD", 2) == 0)
        {
            sscanf(buf, "VD%d", &vd);
        }
        else if (strncmp(buf, "SM0;", 4) == 0)
        {
            sprintf(buf, "SM0%d;", sm0);
            n = write(fd, buf, strlen(buf));
        }
        else if (strncmp(buf, "SM0", 3) == 0)
        {
            sscanf(buf, "SM0%3d", &sm0);
        }
        else if (strncmp(buf, "SM1;", 4) == 0)
        {
            sprintf(buf, "SM1%d;", sm1);
            n = write(fd, buf, strlen(buf));
        }
        else if (strncmp(buf, "SM1", 3) == 0)
        {
            sscanf(buf, "SM1%3d", &sm1);
        }

        else if (strlen(buf) > 0)
        {
            fprintf(stderr, "Unknown command: %s\n", buf);
        }

    }

    return 0;
}
