// NOTE: no cbase.h here on purpose; see tf2bridge_net.h.
#include "tf2bridge_net.h"

#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
typedef SOCKET Sock;
static const Sock kBad = INVALID_SOCKET;
static void CloseSock(Sock s) { closesocket(s); }
static void NonBlocking(Sock s) { u_long on = 1; ioctlsocket(s, FIONBIO, &on); }
#else // POSIX build exists only so the protocol can be tested off-Windows
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int Sock;
static const Sock kBad = -1;
static void CloseSock(Sock s) { close(s); }
static void NonBlocking(Sock s) { fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK); }
#endif

static Sock g_sock = kBad;
static sockaddr_in g_bfbb;
static uint32_t g_sendSeq = 0;
static uint32_t g_lastSeq = 0;
static bool g_haveSeq = false;
static bool g_weaponFiredPending = false;
static uint32_t g_hitscanCount = 0;
static float g_hitscanOrigin[3] = { 0.0f, 0.0f, 0.0f };
static float g_hitscanDir[BRIDGE_MAX_HITSCAN_RAYS][3] = {};
static float g_hitscanRange = 0.0f;

void TF2Bridge_NotifyWeaponFired()
{
    g_weaponFiredPending = true;
}

void TF2Bridge_NotifyHitscanRay(const float origin[3], const float dir[3], float range)
{
    if (g_hitscanCount >= BRIDGE_MAX_HITSCAN_RAYS)
        return;

    if (g_hitscanCount == 0)
    {
        g_hitscanOrigin[0] = origin[0];
        g_hitscanOrigin[1] = origin[1];
        g_hitscanOrigin[2] = origin[2];
        g_hitscanRange = range;
    }

    g_hitscanDir[g_hitscanCount][0] = dir[0];
    g_hitscanDir[g_hitscanCount][1] = dir[1];
    g_hitscanDir[g_hitscanCount][2] = dir[2];
    ++g_hitscanCount;
}

bool TF2Bridge_NetOpen()
{
    if (g_sock != kBad)
        return true;

#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        return false;
#endif

    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_sock == kBad)
        return false;

    sockaddr_in me;
    memset(&me, 0, sizeof(me));
    me.sin_family = AF_INET;
    me.sin_port = htons(BRIDGE_PORT_TF2);
    inet_pton(AF_INET, "127.0.0.1", &me.sin_addr);

    if (bind(g_sock, (sockaddr*)&me, sizeof(me)) != 0)
    {
        CloseSock(g_sock);
        g_sock = kBad;
        return false;
    }

    NonBlocking(g_sock);

    memset(&g_bfbb, 0, sizeof(g_bfbb));
    g_bfbb.sin_family = AF_INET;
    g_bfbb.sin_port = htons(BRIDGE_PORT_BFBB);
    inet_pton(AF_INET, "127.0.0.1", &g_bfbb.sin_addr);
    return true;
}

void TF2Bridge_NetClose()
{
    if (g_sock != kBad)
    {
        CloseSock(g_sock);
        g_sock = kBad;
    }
    g_haveSeq = false;
}

bool TF2Bridge_NetIsOpen()
{
    return g_sock != kBad;
}

bool TF2Bridge_NetRecvState(BridgeStatePacket* out)
{
    if (g_sock == kBad)
        return false;

    bool got = false;
    for (;;)
    {
        BridgeStatePacket p;
        int n = (int)recv(g_sock, (char*)&p, sizeof(p), 0);
        if (n < 0)
            break;
        if (n != (int)sizeof(p) || p.magic != BRIDGE_MAGIC_STATE)
            continue;
        if (g_haveSeq && (int32_t)(p.seq - g_lastSeq) <= 0)
            continue; // reordered or duplicate
        g_lastSeq = p.seq;
        g_haveSeq = true;
        *out = p;
        got = true;
    }
    return got;
}

void TF2Bridge_NetSendIntent(BridgeIntentPacket* pkt)
{
    if (g_sock == kBad)
        return;

    if (g_weaponFiredPending)
    {
        pkt->weaponflags |= BRIDGE_WEAPON_FIRED;
        g_weaponFiredPending = false;
    }

    if (g_hitscanCount > 0)
    {
        pkt->hitscanCount = g_hitscanCount;
        pkt->hitscanRange = g_hitscanRange;
        memcpy(pkt->hitscanOrigin, g_hitscanOrigin, sizeof(g_hitscanOrigin));
        memcpy(pkt->hitscanDir, g_hitscanDir, sizeof(g_hitscanDir));
        g_hitscanCount = 0;
        g_hitscanRange = 0.0f;
        memset(g_hitscanDir, 0, sizeof(g_hitscanDir));
    }

    pkt->magic = BRIDGE_MAGIC_INTENT;
    pkt->seq = ++g_sendSeq;
    sendto(g_sock, (const char*)pkt, sizeof(*pkt), 0, (const sockaddr*)&g_bfbb, sizeof(g_bfbb));
}
