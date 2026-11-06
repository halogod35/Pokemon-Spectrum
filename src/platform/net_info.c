// Pokemon Spectrum multiplayer (PS-022): local IPv4 addresses for the Host Game screen.
// See Platform_GetLanAddresses in include/platform.h.
//
// Linux: getifaddrs() lists every interface address.
// Windows: the "UDP connect" trick: connect() on a UDP socket only picks a route (no packet
// is sent), then getsockname() gives the address of the interface that would be used.
// That is the primary LAN address, which is what the host needs to tell friends.
// Both need only libc / ws2_32 (already linked for ENet), so no new dependency.

#ifdef PORTABLE

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif
#include <string.h>

#include "global.h"
#include "platform.h"

// a.b.c.d from a host-order address.
static u32 FormatIPv4(char *out, u32 outSize, u32 hostOrder)
{
    char tmp[16];
    u32 len = 0;
    s32 shift;

    for (shift = 24; shift >= 0; shift -= 8)
    {
        u32 octet = (hostOrder >> shift) & 0xFF;
        char digits[3];
        u32 n = 0;

        do
        {
            digits[n++] = '0' + (octet % 10);
            octet /= 10;
        } while (octet != 0);
        while (n > 0)
            tmp[len++] = digits[--n];
        if (shift > 0)
            tmp[len++] = '.';
    }
    if (len + 1 > outSize)
        return 0;
    memcpy(out, tmp, len);
    out[len] = '\0';
    return len;
}

// 127.0.0.0/8 (loopback), 169.254.0.0/16 (link-local) and 0.0.0.0 are of no use to friends.
static bool8 IsUsefulAddress(u32 hostOrder)
{
    if (hostOrder == 0)
        return FALSE;
    if ((hostOrder >> 24) == 127)
        return FALSE;
    if ((hostOrder >> 16) == 0xA9FE)
        return FALSE;
    return TRUE;
}

// Appends " a.b.c.d" (no leading space for the first). FALSE if it doesn't fit.
static bool8 AppendAddress(char *out, u32 outSize, u32 *used, u32 count, u32 hostOrder)
{
    u32 len;

    if (count > 0)
    {
        if (*used + 1 >= outSize)
            return FALSE;
        out[(*used)++] = ' ';
        out[*used] = '\0';
    }
    len = FormatIPv4(out + *used, outSize - *used, hostOrder);
    if (len == 0)
        return FALSE;
    *used += len;
    return TRUE;
}

u32 Platform_GetLanAddresses(char *out, u32 outSize, u32 max)
{
    u32 count = 0;
    u32 used = 0;

    if (out == NULL || outSize == 0)
        return 0;
    out[0] = '\0';
    if (max > PLATFORM_LAN_ADDRESSES_MAX)
        max = PLATFORM_LAN_ADDRESSES_MAX;

#ifdef _WIN32
    {
        WSADATA wsa;
        SOCKET sock;
        struct sockaddr_in remote;
        struct sockaddr_in local;
        int localLen = sizeof(local);

        if (max == 0 || WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
            return 0;
        sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock != INVALID_SOCKET)
        {
            memset(&remote, 0, sizeof(remote));
            remote.sin_family = AF_INET;
            remote.sin_port = htons(53);
            remote.sin_addr.s_addr = htonl(0x08080808); // any public address; nothing is sent
            memset(&local, 0, sizeof(local));
            if (connect(sock, (struct sockaddr *)&remote, sizeof(remote)) == 0
             && getsockname(sock, (struct sockaddr *)&local, &localLen) == 0
             && local.sin_family == AF_INET)
            {
                u32 addr = ntohl(local.sin_addr.s_addr);

                if (IsUsefulAddress(addr) && AppendAddress(out, outSize, &used, count, addr))
                    count++;
            }
            closesocket(sock);
        }
        WSACleanup();
    }
#else
    {
        struct ifaddrs *list = NULL;
        struct ifaddrs *it;

        if (getifaddrs(&list) != 0)
            return 0;
        for (it = list; it != NULL && count < max; it = it->ifa_next)
        {
            u32 addr;

            if (it->ifa_addr == NULL || it->ifa_addr->sa_family != AF_INET)
                continue;
            if (!(it->ifa_flags & IFF_UP) || (it->ifa_flags & IFF_LOOPBACK))
                continue;
            addr = ntohl(((struct sockaddr_in *)it->ifa_addr)->sin_addr.s_addr);
            if (!IsUsefulAddress(addr))
                continue;
            if (!AppendAddress(out, outSize, &used, count, addr))
                break;
            count++;
        }
        freeifaddrs(list);
    }
#endif
    return count;
}

#endif // PORTABLE
