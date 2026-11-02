/*
 * Network play on PC: UDP over the local network (platform.h, plat_net_*).
 *
 * Each copy of the game takes the first free port of NET_PORT_FIRST..+NET_PORTS-1,
 * so several copies can run on one machine. Broadcasts go to every port of that
 * range, on the local network (255.255.255.255) and on this machine (127.0.0.1,
 * which some systems leave out of broadcasts).
 */
#include "platform/platform.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
typedef SOCKET sock_t;
#  define BAD_SOCK INVALID_SOCKET
#  define close_sock closesocket
#else
#  include <sys/types.h>
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <unistd.h>
#  include <fcntl.h>
#  include <time.h>
typedef int sock_t;
#  define BAD_SOCK (-1)
#  define close_sock close
#endif

#define NET_PORT_FIRST 41290
#define NET_PORTS      8

static sock_t sock = BAD_SOCK;
static int my_port;

static void to_addr(const PlatNetAddr *a, struct sockaddr_in *sa)
{
    memset(sa, 0, sizeof *sa);
    sa->sin_family = AF_INET;
    memcpy(&sa->sin_addr.s_addr, a->b, 4);
    memcpy(&sa->sin_port, a->b + 4, 2);
}

static void from_addr(const struct sockaddr_in *sa, PlatNetAddr *a)
{
    memset(a, 0, sizeof *a);
    memcpy(a->b, &sa->sin_addr.s_addr, 4);
    memcpy(a->b + 4, &sa->sin_port, 2);
}

int plat_net_open(void)
{
    int port, yes = 1;
    if (sock != BAD_SOCK) return 0;
#ifdef _WIN32
    {
        static int started;
        WSADATA wd;
        if (!started && WSAStartup(MAKEWORD(2, 2), &wd) != 0) return -1;
        started = 1;
    }
#endif
    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == BAD_SOCK) return -1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, (const char *)&yes, sizeof yes);
    for (port = NET_PORT_FIRST; port < NET_PORT_FIRST + NET_PORTS; port++) {
        struct sockaddr_in sa;
        memset(&sa, 0, sizeof sa);
        sa.sin_family = AF_INET;
        sa.sin_addr.s_addr = htonl(INADDR_ANY);
        sa.sin_port = htons((unsigned short)port);
        if (bind(sock, (struct sockaddr *)&sa, sizeof sa) == 0) break;
    }
    if (port == NET_PORT_FIRST + NET_PORTS) { close_sock(sock); sock = BAD_SOCK; return -2; }
    my_port = port;
#ifdef _WIN32
    {
        u_long nb = 1;
        ioctlsocket(sock, FIONBIO, &nb);
        /* (Windows reports an earlier send to a closed port as an error on the next
         * receive; that must not end the session) */
        {
            BOOL off = FALSE;
            DWORD got = 0;
            WSAIoctl(sock, _WSAIOW(IOC_VENDOR, 12) /* SIO_UDP_CONNRESET */, &off, sizeof off, NULL, 0, &got, NULL, NULL);
        }
    }
#else
    fcntl(sock, F_SETFL, fcntl(sock, F_GETFL, 0) | O_NONBLOCK);
#endif
    return 0;
}

void plat_net_close(void)
{
    if (sock != BAD_SOCK) close_sock(sock);
    sock = BAD_SOCK;
}

int plat_net_send(const PlatNetAddr *to, const void *data, int len)
{
    struct sockaddr_in sa;
    if (sock == BAD_SOCK) return -1;
    to_addr(to, &sa);
    return sendto(sock, (const char *)data, len, 0, (struct sockaddr *)&sa, sizeof sa) == len ? 0 : -1;
}

int plat_net_broadcast(const void *data, int len)
{
    int port, ok = -1;
    if (sock == BAD_SOCK) return -1;
    for (port = NET_PORT_FIRST; port < NET_PORT_FIRST + NET_PORTS; port++) {
        struct sockaddr_in sa;
        int k;
        for (k = 0; k < 2; k++) {
            memset(&sa, 0, sizeof sa);
            sa.sin_family = AF_INET;
            sa.sin_addr.s_addr = htonl(k ? INADDR_LOOPBACK : INADDR_BROADCAST);
            sa.sin_port = htons((unsigned short)port);
            if (k && port == my_port) continue;
            if (sendto(sock, (const char *)data, len, 0, (struct sockaddr *)&sa, sizeof sa) == len) ok = 0;
        }
    }
    return ok;
}

int plat_net_recv(PlatNetAddr *from, void *buf, int cap)
{
    struct sockaddr_in sa;
#ifdef _WIN32
    int sl = sizeof sa;
#else
    socklen_t sl = sizeof sa;
#endif
    int n;
    if (sock == BAD_SOCK) return 0;
    for (;;) {
        n = (int)recvfrom(sock, (char *)buf, cap, 0, (struct sockaddr *)&sa, &sl);
        if (n > 0) break;
        if (n == 0) return 0;
#ifdef _WIN32
        if (WSAGetLastError() == WSAECONNRESET || WSAGetLastError() == WSAEMSGSIZE) continue;
#endif
        return 0;
    }
    from_addr(&sa, from);
    return n;
}

int plat_net_addr_equal(const PlatNetAddr *a, const PlatNetAddr *b)
{
    return memcmp(a->b, b->b, 6) == 0;
}

void plat_net_addr_text(const PlatNetAddr *a, char *out, int size)
{
    unsigned short port;
    memcpy(&port, a->b + 4, 2);
    snprintf(out, (size_t)size, "%u.%u.%u.%u:%u", a->b[0], a->b[1], a->b[2], a->b[3], ntohs(port));
}

void plat_net_name(char *out, int size)
{
    char name[256] = "";
#ifdef _WIN32
    DWORD n = sizeof name;
    if (!GetComputerNameA(name, &n)) name[0] = 0;
#else
    if (gethostname(name, sizeof name - 1) != 0) name[0] = 0;
#endif
    name[sizeof name - 1] = 0;
    snprintf(out, (size_t)size, "%s", name[0] ? name : "ToeJam");
}

uint32_t plat_ms(void)
{
#ifdef _WIN32
    return (uint32_t)GetTickCount();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
#endif
}
