// --mp-selftest (PS-011): runs without a window or a save, prints one line per
// group and "[MP] selftest OK" / "[MP] selftest FAILED (<n>)", and returns the
// process exit status (0 / 1).
//
// PS-011 covers: wire helpers, the IPv4 check, transport argument checks, and a
// loopback host + client in one process exchanging DEBUG_ECHO packets on both
// channels. PS-012 / PS-013 add the parser fuzzing of protocol.md section 8.3 here.

#include <string.h>
#include "mp/mp.h"
#include "mp/mp_internal.h"
#include "mp/mp_protocol.h"
#include "mp/mp_selftest.h"
#include "mp/mp_summary.h"
#include "mp/mp_transport.h"
#include "mp/mp_wire.h"

#define SELFTEST_INSTANCE_SERVER    0
#define SELFTEST_INSTANCE_CLIENT    1
#define SELFTEST_PORT_FIRST         27590
#define SELFTEST_PORT_LAST          27599
#define SELFTEST_TIMEOUT_MS         5000
#define SELFTEST_ECHO_COUNT         5

static u32 sFailures;

static struct
{
    bool8 serverConnected;
    bool8 serverDisconnected;
    bool8 clientConnected;
    bool8 clientDisconnected;
    u8 serverPeer;
    u32 reliableEchoes;
    u32 unreliableEchoes;
    u32 badPackets;
} sLoop;

#define CHECK(cond, what)                                   \
    do                                                      \
    {                                                       \
        if (!(cond))                                        \
        {                                                   \
            MP_LOG("selftest: FAIL: %s (%s:%d)\n", what, __FILE__, __LINE__); \
            sFailures++;                                    \
        }                                                   \
    } while (0)

// ---------------------------------------------------------------------------
// Wire helpers
// ---------------------------------------------------------------------------

static void TestWire(void)
{
    u8 buf[16];
    u8 bytes[4];
    struct MpWriter w;
    struct MpReader r;
    u32 failuresBefore = sFailures;

    MpWriter_Init(&w, buf, sizeof(buf));
    MpWriteU8(&w, 0xAB);
    MpWriteU16(&w, 0x1234);
    MpWriteU32(&w, 0xDEADBEEF);
    MpWriteS16(&w, -2);
    CHECK(!w.error && w.pos == 9, "writer basic");
    CHECK(buf[0] == 0xAB && buf[1] == 0x34 && buf[2] == 0x12, "writer little-endian u16");
    CHECK(buf[3] == 0xEF && buf[4] == 0xBE && buf[5] == 0xAD && buf[6] == 0xDE, "writer little-endian u32");
    CHECK(buf[7] == 0xFE && buf[8] == 0xFF, "writer s16");

    MpReader_Init(&r, buf, 9);
    CHECK(MpReadU8(&r) == 0xAB, "reader u8");
    CHECK(MpReadU16(&r) == 0x1234, "reader u16");
    CHECK(MpReadU32(&r) == 0xDEADBEEF, "reader u32");
    CHECK(MpReadS16(&r) == -2, "reader s16");
    CHECK(!r.error && MpReader_Remaining(&r) == 0, "reader consumed all");
    CHECK(MpReadU8(&r) == 0 && r.error, "reader past end sets error");
    CHECK(MpReadU32(&r) == 0 && r.error, "reader error is sticky");

    // Truncated u32 must not read partial data
    MpReader_Init(&r, buf, 3);
    MpReadU8(&r);
    CHECK(MpReadU32(&r) == 0 && r.error, "reader truncated u32");

    // Huge length must not overflow the bounds check
    MpReader_Init(&r, buf, 4);
    MpReadU8(&r);
    memset(bytes, 0x55, sizeof(bytes));
    MpReadBytes(&r, bytes, 4);
    CHECK(r.error && bytes[0] == 0 && bytes[3] == 0, "reader bytes past end zero-fills");

    MpWriter_Init(&w, buf, 3);
    MpWriteU16(&w, 1);
    MpWriteU16(&w, 2);
    CHECK(w.error && w.pos == 2, "writer stops at capacity");
    MpWriteU8(&w, 3);
    CHECK(w.error && w.pos == 2, "writer error is sticky");

    MpReader_Init(&r, NULL, 100);
    CHECK(MpReadU8(&r) == 0 && r.error, "reader NULL buffer");

    if (sFailures == failuresBefore)
        MP_LOG("selftest: wire helpers OK\n");
}

static void TestIPv4(void)
{
    u32 failuresBefore = sFailures;

    CHECK(MpTransport_IsValidIPv4("127.0.0.1"), "ipv4 127.0.0.1");
    CHECK(MpTransport_IsValidIPv4("255.255.255.255"), "ipv4 255.255.255.255");
    CHECK(MpTransport_IsValidIPv4("0.0.0.0"), "ipv4 0.0.0.0");
    CHECK(!MpTransport_IsValidIPv4(""), "ipv4 empty");
    CHECK(!MpTransport_IsValidIPv4(NULL), "ipv4 NULL");
    CHECK(!MpTransport_IsValidIPv4("256.0.0.1"), "ipv4 octet > 255");
    CHECK(!MpTransport_IsValidIPv4("1.2.3"), "ipv4 three octets");
    CHECK(!MpTransport_IsValidIPv4("1.2.3.4.5"), "ipv4 five octets");
    CHECK(!MpTransport_IsValidIPv4("1..3.4"), "ipv4 empty octet");
    CHECK(!MpTransport_IsValidIPv4("1.2.3.4."), "ipv4 trailing dot");
    CHECK(!MpTransport_IsValidIPv4("0001.2.3.4"), "ipv4 four digits");
    CHECK(!MpTransport_IsValidIPv4("localhost"), "ipv4 hostname");
    CHECK(!MpTransport_IsValidIPv4("1.2.3.4:27500"), "ipv4 with port");
    CHECK(!MpTransport_IsValidIPv4(" 1.2.3.4"), "ipv4 leading space");

    if (sFailures == failuresBefore)
        MP_LOG("selftest: IPv4 check OK\n");
}

// ---------------------------------------------------------------------------
// Loopback transport
// ---------------------------------------------------------------------------

static u16 WriteEcho(u8 *buf, u16 cap, u8 index, u8 channel)
{
    struct MpWriter w;
    u8 i;

    MpWriter_Init(&w, buf, cap);
    MpWriteU8(&w, MP_MSG_DEBUG_ECHO);
    MpWriteU8(&w, MP_PROTOCOL_VERSION);
    MpWriteU16(&w, 8);
    for (i = 0; i < 8; i++)
        MpWriteU8(&w, (u8)(index * 16 + channel * 8 + i));
    return w.error ? 0 : (u16)w.pos;
}

// Returns the echo index, or 0xFF if the packet is not a well-formed DEBUG_ECHO.
static u8 ParseEcho(const u8 *data, u32 len, u8 channel)
{
    struct MpReader r;
    u8 type, version, index, i;
    u16 length;

    MpReader_Init(&r, data, len);
    type = MpReadU8(&r);
    version = MpReadU8(&r);
    length = MpReadU16(&r);
    if (r.error || type != MP_MSG_DEBUG_ECHO || version != MP_PROTOCOL_VERSION
     || length != 8 || MpReader_Remaining(&r) != length)
        return 0xFF;
    index = MpReadU8(&r) / 16;
    for (i = 1; i < 8; i++)
    {
        if (MpReadU8(&r) != (u8)(index * 16 + channel * 8 + i))
            return 0xFF;
    }
    return r.error ? 0xFF : index;
}

static void OnServerEvent(const struct MpTransportEvent *ev)
{
    switch (ev->type)
    {
    case MP_TRANSPORT_EV_CONNECT:
        sLoop.serverConnected = TRUE;
        sLoop.serverPeer = ev->peer;
        break;
    case MP_TRANSPORT_EV_DISCONNECT:
        sLoop.serverDisconnected = TRUE;
        break;
    case MP_TRANSPORT_EV_RECEIVE:
        if (ParseEcho(ev->data, ev->length, ev->channel) == 0xFF)
        {
            sLoop.badPackets++;
            break;
        }
        // Echo back on the same channel, same reliability.
        MpTransport_Send(ev->peer, ev->channel, ev->data, (u16)ev->length, ev->channel == MP_CHANNEL_CONTROL);
        break;
    }
}

static void OnClientEvent(const struct MpTransportEvent *ev)
{
    switch (ev->type)
    {
    case MP_TRANSPORT_EV_CONNECT:
        sLoop.clientConnected = TRUE;
        break;
    case MP_TRANSPORT_EV_DISCONNECT:
        sLoop.clientDisconnected = TRUE;
        break;
    case MP_TRANSPORT_EV_RECEIVE:
        if (ParseEcho(ev->data, ev->length, ev->channel) == 0xFF)
            sLoop.badPackets++;
        else if (ev->channel == MP_CHANNEL_CONTROL)
            sLoop.reliableEchoes++;
        else
            sLoop.unreliableEchoes++;
        break;
    }
}

static void PumpBoth(void)
{
    MpTransport_SelectInstance(SELFTEST_INSTANCE_SERVER);
    MpTransport_Poll(OnServerEvent);
    MpTransport_Flush();
    MpTransport_SelectInstance(SELFTEST_INSTANCE_CLIENT);
    MpTransport_Poll(OnClientEvent);
    MpTransport_Flush();
}

// Pumps until *flag is TRUE or the deadline passes. Returns *flag.
static bool8 PumpUntil(const bool8 *flag, u32 deadline)
{
    while (!*flag && (s32)(deadline - MpTransport_GetTimeMs()) > 0)
        PumpBoth();
    return *flag;
}

static void TestLoopback(void)
{
    u8 buf[MP_MAX_PACKET_SIZE + 1];
    u16 port;
    u16 len;
    u8 i;
    u32 deadline;
    bool8 listening = FALSE;
    bool8 done;
    u32 failuresBefore = sFailures;

    memset(&sLoop, 0, sizeof(sLoop));

    MpTransport_SelectInstance(SELFTEST_INSTANCE_SERVER);
    for (port = SELFTEST_PORT_FIRST; port <= SELFTEST_PORT_LAST && !listening; port++)
        listening = MpTransport_Listen(port, MP_MAX_PLAYERS);
    port--;
    CHECK(listening, "listen on a port in 27590..27599");
    if (!listening)
        return;
    CHECK(!MpTransport_Listen(port, MP_MAX_PLAYERS), "second listen in the same instance is refused");

    MpTransport_SelectInstance(SELFTEST_INSTANCE_CLIENT);
    CHECK(!MpTransport_Connect("localhost", port), "connect refuses hostnames");
    CHECK(!MpTransport_IsOpen(), "refused connect leaves no host");
    CHECK(MpTransport_Connect("127.0.0.1", port), "connect to 127.0.0.1");
    CHECK(!MpTransport_Send(0, MP_CHANNEL_CONTROL, buf, 1, TRUE), "send before connected is refused");

    deadline = MpTransport_GetTimeMs() + SELFTEST_TIMEOUT_MS;
    CHECK(PumpUntil(&sLoop.clientConnected, deadline), "client connected");
    CHECK(PumpUntil(&sLoop.serverConnected, deadline), "server saw the connection");
    if (!sLoop.clientConnected || !sLoop.serverConnected)
        return;
    MP_LOG("selftest: loopback connected on port %u\n", (unsigned)port);

    // Argument checks on a live peer
    MpTransport_SelectInstance(SELFTEST_INSTANCE_CLIENT);
    memset(buf, 0, sizeof(buf));
    CHECK(!MpTransport_Send(0, MP_CHANNEL_CONTROL, buf, MP_MAX_PACKET_SIZE + 1, TRUE), "oversized send refused");
    CHECK(!MpTransport_Send(0, MP_CHANNEL_COUNT, buf, 4, TRUE), "bad channel refused");
    CHECK(!MpTransport_Send(7, MP_CHANNEL_CONTROL, buf, 4, TRUE), "bad peer refused");
    CHECK(!MpTransport_Send(0, MP_CHANNEL_CONTROL, NULL, 4, TRUE), "NULL data refused");

    for (i = 0; i < SELFTEST_ECHO_COUNT; i++)
    {
        len = WriteEcho(buf, sizeof(buf), i, MP_CHANNEL_CONTROL);
        CHECK(len != 0 && MpTransport_Send(0, MP_CHANNEL_CONTROL, buf, len, TRUE), "reliable send");
        len = WriteEcho(buf, sizeof(buf), i, MP_CHANNEL_PRESENCE);
        CHECK(len != 0 && MpTransport_Send(0, MP_CHANNEL_PRESENCE, buf, len, FALSE), "unreliable send");
    }

    done = FALSE;
    while (!done && (s32)(deadline - MpTransport_GetTimeMs()) > 0)
    {
        PumpBoth();
        done = (sLoop.reliableEchoes >= SELFTEST_ECHO_COUNT);
    }
    CHECK(sLoop.reliableEchoes == SELFTEST_ECHO_COUNT, "all reliable echoes came back");
    CHECK(sLoop.badPackets == 0, "no malformed echo");
    MP_LOG("selftest: echoes reliable %u/%u, unreliable %u/%u (unreliable loss is allowed)\n",
           (unsigned)sLoop.reliableEchoes, (unsigned)SELFTEST_ECHO_COUNT,
           (unsigned)sLoop.unreliableEchoes, (unsigned)SELFTEST_ECHO_COUNT);

    // Graceful disconnect from the client; the server must see it.
    MpTransport_SelectInstance(SELFTEST_INSTANCE_CLIENT);
    MpTransport_Disconnect(0);
    deadline = MpTransport_GetTimeMs() + SELFTEST_TIMEOUT_MS;
    CHECK(PumpUntil(&sLoop.serverDisconnected, deadline), "server saw the disconnect");
    CHECK(PumpUntil(&sLoop.clientDisconnected, deadline), "client disconnect completed");

    MpTransport_SelectInstance(SELFTEST_INSTANCE_CLIENT);
    MpTransport_Close();
    CHECK(!MpTransport_IsOpen(), "client closed");
    MpTransport_SelectInstance(SELFTEST_INSTANCE_SERVER);
    MpTransport_Close();
    CHECK(!MpTransport_IsOpen(), "server closed");

    if (sFailures == failuresBefore)
        MP_LOG("selftest: loopback transport OK\n");
}

s32 MP_RunSelftest(void)
{
    sFailures = 0;
    MP_LOG("selftest: build %s, protocol %u, hash 0x%08X\n", MP_GetBuildId(),
           (unsigned)MP_PROTOCOL_VERSION, (unsigned)MP_GetBuildHash());

    TestWire();
    TestIPv4();
    // PS-013: summary round trip / malformed buffers / name rules. Uses its own
    // in-memory fixture save blocks, so no .sav file is needed.
    if (!MpSummary_SelfTest())
    {
        MP_LOG("selftest: FAIL: player summary (see the lines above)\n");
        sFailures++;
    }
    if (!MpTransport_Init())
    {
        MP_LOG("selftest: FAIL: ENet initialisation\n");
        sFailures++;
    }
    else
    {
        TestLoopback();
    }
    // PS-012: protocol.md 8.3 parser fuzzing, then the in-process session scenario.
    sFailures += MpSelftest_Parsers();
    sFailures += MpSelftest_Session();
    // PS-030: presence parser, receive paths and the host + clients presence scenario.
    sFailures += MpSelftest_Presence();
    // PS-031: remote-avatar decision logic (pure functions).
    sFailures += MpSelftest_Avatar();
    // PS-041: INTERACT parsers and the request / accept / decline / timeout scenario.
    sFailures += MpSelftest_Interact();
    // PS-046: virtual link cable (parsers + the real link stack on two in-process game instances).
    sFailures += MpSelftest_Link();
    MpTransport_Shutdown();

    if (sFailures == 0)
    {
        MP_LOG("selftest OK\n");
        return 0;
    }
    MP_LOG("selftest FAILED (%u)\n", (unsigned)sFailures);
    return 1;
}
