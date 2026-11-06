// Multiplayer session: lifecycle and per-frame pump (design.md sections 3 and 5).
//
// PS-011 skeleton: MP_Init / MP_Update / MP_Shutdown / MP_SetDebugLogging only.
// The session API (MP_Host, MP_Join, MP_GetState, events, ...) is PS-012's and
// is added to this file there; until then UI cards may stub it in mp_stub.c.

#include "mp/mp.h"
#include "mp/mp_internal.h"
#include "mp/mp_transport.h"

bool8 gMpDebugLogging = FALSE;

static bool8 sMpInitialized = FALSE;

// PS-012 replaces this with the real dispatch (server/client roles).
static void MpOnTransportEvent(const struct MpTransportEvent *ev)
{
    switch (ev->type)
    {
    case MP_TRANSPORT_EV_CONNECT:
        MP_DLOG("peer %u connected (no session yet, ignored)\n", (unsigned)ev->peer);
        break;
    case MP_TRANSPORT_EV_DISCONNECT:
        MP_DLOG("peer %u disconnected\n", (unsigned)ev->peer);
        break;
    case MP_TRANSPORT_EV_RECEIVE:
        MP_DLOG("peer %u: %u bytes on channel %u dropped (no session yet)\n",
                (unsigned)ev->peer, (unsigned)ev->length, (unsigned)ev->channel);
        break;
    }
}

void MP_Init(void)
{
    if (sMpInitialized)
        return;
    sMpInitialized = TRUE;
    // No ENet call and no socket here: the transport initialises lazily on the
    // first Listen/Connect, so single-player never touches the network.
    MpBuild_Init();
}

void MP_SetDebugLogging(bool8 enabled)
{
    gMpDebugLogging = enabled ? TRUE : FALSE;
    if (gMpDebugLogging)
        MP_LOG("debug logging on (build %s, protocol %u)\n", MP_GetBuildId(), (unsigned)MpBuild_GetProtocolVersion());
}

void MP_Update(void)
{
    if (!sMpInitialized || !MpTransport_IsOpen())
        return;
    MpTransport_Poll(MpOnTransportEvent);
    MpTransport_Flush();
}

void MP_Shutdown(void)
{
    if (!sMpInitialized)
        return;
    sMpInitialized = FALSE;
    // PS-012: send BYE and service for up to 250 ms before this.
    MpTransport_Shutdown();
}
