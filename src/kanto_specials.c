// PS-100: script specials for the Kanto story import (epic PS-099). The imported pokefirered scripts call
// their specials through tools/kanto_import/specials.tsv: a special with the same name here is used as it is,
// an Emerald twin is renamed, and the rest come here, either as stubs (FRLG-only systems: quest log, help
// system, fame checker, Trainer Tower, walk-away message boxes) or as ports of pokefirered's function, each
// marked `// PS-100:` with the FRLG name. Every special returns a 16- or 32-bit type (PS-090, make checkspecials).
#include "global.h"
#include "event_data.h"
#include "fieldmap.h"
#include "field_camera.h"
#include "field_player_avatar.h"
#include "kanto_specials.h"
#include "pokedex.h"
#include "pokemon.h"
#include "task.h"
#include "constants/metatile_labels.h"
#include "constants/vars.h"

// --- stubs ---------------------------------------------------------------------------------------------
// FRLG-only systems that do nothing here (specials.tsv says which pokefirered special maps to which stub)

void KantoStub_Nop(void)
{
}

u16 KantoStub_True(void)
{
    return TRUE;
}

u16 KantoStub_False(void)
{
    return FALSE;
}

// --- ports ---------------------------------------------------------------------------------------------

// PS-100: pokefirered's PcTurnOnUpdateMetatileId (src/field_specials.c). VAR_0x8004: 0 the generic building PC,
// 1 / 2 the player's bedroom PC. The metatile ids are pokefirered's own (the prefix re-pack keeps them).
static void KantoPcSetMetatile(bool16 off)
{
    u16 metatileId;
    s8 deltaX = 0;
    s8 deltaY = 0;

    switch (GetPlayerFacingDirection())
    {
    case DIR_NORTH:
        deltaX = 0;
        deltaY = -1;
        break;
    case DIR_WEST:
        deltaX = -1;
        deltaY = -1;
        break;
    case DIR_EAST:
        deltaX = 1;
        deltaY = -1;
        break;
    }
    if (gSpecialVar_0x8004 == 0)
        metatileId = off ? METATILE_KantoBuilding_PC_Off : METATILE_KantoBuilding_PC_On;
    else
        metatileId = off ? METATILE_KantoGenericBuilding1_PlayersPCOff : METATILE_KantoGenericBuilding1_PlayersPCOn;
    MapGridSetMetatileIdAt(gSaveBlock1Ptr->pos.x + deltaX + MAP_OFFSET, gSaveBlock1Ptr->pos.y + deltaY + MAP_OFFSET, metatileId | MAPGRID_COLLISION_MASK);
}

#define tState data[0]
#define tTimer data[1]

// PS-100: pokefirered's Task_AnimatePcTurnOn: the screen flickers on, off, on, off, on (every 6 frames)
static void Task_KantoAnimatePcTurnOn(u8 taskId)
{
    s16 *data = gTasks[taskId].data;

    if (tTimer == 6)
    {
        KantoPcSetMetatile(tState & 1);
        DrawWholeMapView();
        tTimer = 0;
        tState++;
        if (tState == 5)
            DestroyTask(taskId);
    }
    tTimer++;
}

// PS-100: pokefirered's AnimatePcTurnOn
void KantoAnimatePcTurnOn(void)
{
    u8 taskId;

    if (FuncIsActiveTask(Task_KantoAnimatePcTurnOn) != TRUE)
    {
        taskId = CreateTask(Task_KantoAnimatePcTurnOn, 8);
        gTasks[taskId].tState = 0;
        gTasks[taskId].tTimer = 0;
    }
}

#undef tState
#undef tTimer

// PS-100: pokefirered's AnimatePcTurnOff
void KantoAnimatePcTurnOff(void)
{
    KantoPcSetMetatile(TRUE);
    DrawWholeMapView();
}

// PS-100: pokefirered's GetPokedexCount. VAR_0x8004 0: the Kanto Pokédex (the first 151), else the National one.
// VAR_0x8005 = seen, VAR_0x8006 = owned; returns whether the National Pokédex is enabled.
u16 KantoGetPokedexCount(void)
{
    if (gSpecialVar_0x8004 == 0)
    {
        gSpecialVar_0x8005 = GetKantoPokedexCount(FLAG_GET_SEEN);
        gSpecialVar_0x8006 = GetKantoPokedexCount(FLAG_GET_CAUGHT);
    }
    else
    {
        gSpecialVar_0x8005 = GetNationalPokedexCount(FLAG_GET_SEEN);
        gSpecialVar_0x8006 = GetNationalPokedexCount(FLAG_GET_CAUGHT);
    }
    return IsNationalPokedexEnabled();
}

// PS-100: pokefirered's DaisyMassageServices: Daisy's massage raises the chosen Pokémon's friendship by
// FRIENDSHIP_EVENT_MASSAGE (+3 at every level in pokefirered's sFriendshipEventDeltas; Emerald has no such
// event, so the delta is applied here) and resets the 500-step cooldown kept by Kanto_UpdateStepCounters().
#define KANTO_MASSAGE_FRIENDSHIP_DELTA 3
#define KANTO_MASSAGE_COOLDOWN_STEPS   500

void KantoDaisyMassageServices(void)
{
    struct Pokemon *mon;
    u16 friendship;

    if (gSpecialVar_0x8004 >= PARTY_SIZE)
        return;
    mon = &gPlayerParty[gSpecialVar_0x8004];
    friendship = GetMonData(mon, MON_DATA_FRIENDSHIP);
    friendship += KANTO_MASSAGE_FRIENDSHIP_DELTA;
    if (friendship > MAX_FRIENDSHIP)
        friendship = MAX_FRIENDSHIP;
    SetMonData(mon, MON_DATA_FRIENDSHIP, &friendship);
    VarSet(VAR_KANTO_MASSAGE_COOLDOWN_STEP_COUNTER, 0);
}

// PS-100: pokefirered's IncrementMassageCooldownStepCounter (field_specials.c), called from the field step
// counter (field_control_avatar.c). The var is a plain counter, whatever region the player walks in.
void Kanto_UpdateStepCounters(void)
{
    u16 count = VarGet(VAR_KANTO_MASSAGE_COOLDOWN_STEP_COUNTER);

    if (count < KANTO_MASSAGE_COOLDOWN_STEPS)
        VarSet(VAR_KANTO_MASSAGE_COOLDOWN_STEP_COUNTER, count + 1);
}
