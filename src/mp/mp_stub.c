// Pokemon Spectrum multiplayer: TEMPORARY stubs of the PS-012 session API.
//
// deleted by PS-012
//
// PS-012 deletes this whole file in the same change that adds the real session code
// (BOARD.md "Stub ownership"; design.md section 9). If both ever exist, the duplicate
// symbols fail the link on purpose.
//
// Only the functions the menu cards (PS-020, PS-021) call are stubbed here. Lifecycle
// (MP_Init/MP_Update/MP_Shutdown/MP_SetDebugLogging) belongs to PS-011 and the player
// summary functions to PS-013; they must not be defined here.
//
// The stubs pretend that hosting and joining start fine but never change the state, so
// the menus can be exercised and the game simply continues single-player.

#include "global.h"
#include "mp/mp.h"

// Used by: PS-020 (main menu HOST GAME).
bool8 MP_Host(u16 port)
{
    DBGPRINTF("[MP] stub: MP_Host(%u) - no network yet (PS-012), continuing single-player\n",
              (unsigned int)(port == 0 ? MP_DEFAULT_PORT : port));
    return TRUE;
}

// Used by: PS-021 (Join Game IP entry screen, on confirm).
bool8 MP_Join(const char *ip, u16 port)
{
    DBGPRINTF("[MP] stub: MP_Join(%s, %u) - no network yet (PS-012), continuing single-player\n",
              ip != NULL ? ip : "(null)", (unsigned int)(port == 0 ? MP_DEFAULT_PORT : port));
    return TRUE;
}

// Used by: PS-020 and PS-021 (precondition check before MP_Host / MP_Join).
u8 MP_GetState(void)
{
    return MP_STATE_OFFLINE;
}
