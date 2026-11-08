#ifndef GUARD_MP_BATTLE_H
#define GUARD_MP_BATTLE_H

// Direct link battle from the overworld (PS-045, src/mp/mp_battle.c): the pure parts, so the
// selftest can run them without a game. The game side is in mp.h / mp_menu.h.

// MP_BattleCheckParty results.
enum
{
    MP_BATTLE_PARTY_OK,
    MP_BATTLE_PARTY_NO_MONS,        // not one POKEMON that isn't an egg
    MP_BATTLE_PARTY_NEED_TWO,       // DOUBLE: fewer than two usable POKEMON (not an egg, HP > 0)
};

// The Cable Club's rules (cable_club.inc, GetMonsStateToDoubles), on `count` party entries.
// speciesOrEgg[i] is MON_DATA_SPECIES_OR_EGG (SPECIES_NONE for an empty slot), hp[i] is MON_DATA_HP.
// SINGLE: at least one POKEMON that isn't an egg (the party is fully healed before the battle).
// DOUBLE: at least two that aren't eggs and have HP left, checked before the heal, as the Cable
// Club does.
u8 MP_BattleCheckParty(const u16 *speciesOrEgg, const u16 *hp, u32 count, bool8 isDouble);

// The linkup part of the start flow (after the save, until both have exchanged link players).
enum
{
    MP_BATTLE_FLOW_WAIT_GROUP,      // MP_LinkJoin sent; waiting for GROUPED
    MP_BATTLE_FLOW_WAIT_PLAYERS,    // OpenLinkTimed done; waiting for both on the cable
    MP_BATTLE_FLOW_WAIT_EXCHANGE,   // waiting for the link player exchange
    MP_BATTLE_FLOW_LINKED,          // done: SaveLinkPlayers(2), then the battle starts
    MP_BATTLE_FLOW_FAILED,
};

// What the caller has to do after a step.
enum
{
    MP_BATTLE_ACT_NONE,
    MP_BATTLE_ACT_OPEN_LINK,        // SetLocalLinkPlayerId, gLinkType, OpenLinkTimed
    MP_BATTLE_ACT_ADVANCE_LINK,     // link master: CheckShouldAdvanceLinkState
    MP_BATTLE_ACT_LINKED,           // SaveLinkPlayers(2), then start the battle
    MP_BATTLE_ACT_FAIL,             // cancel (CloseLink, MP_LinkLeave, message)
};

#define MP_BATTLE_GROUP_TIMEOUT_FRAMES  (60 * 10)   // both players must ask for the group within ~10 s
#define MP_BATTLE_LINKUP_TIMEOUT_FRAMES (60 * 15)   // the cable handshake and the player exchange
#define MP_BATTLE_MASTER_DELAY_FRAMES   30          // as the trade: let both see two players first

struct MpBattleFlow
{
    u8 state;       // MP_BATTLE_FLOW_*
    u16 timer;
};

struct MpBattleFlowInput
{
    bool8 sessionActive;    // MP_IsActive()
    bool8 partnerPresent;   // MP_GetPlayer(partner) != NULL
    u8 linkState;           // MP_LinkGetState()
    u8 linkPlayerCount;     // GetLinkPlayerCount_2()
    bool8 isLinkMaster;     // IsLinkMaster()
    u8 exchangeStatus;      // GetLinkPlayerDataExchangeStatusTimed(2, 2) (only read in WAIT_EXCHANGE)
};

void MP_BattleFlowInit(struct MpBattleFlow *flow);
u8 MP_BattleFlowStep(struct MpBattleFlow *flow, const struct MpBattleFlowInput *in); // MP_BATTLE_ACT_*

// MP_INTERACT_BATTLE / MP_INTERACT_BATTLE_DOUBLE -> USING_SINGLE_BATTLE / USING_DOUBLE_BATTLE
// (constants/cable_club.h); 0 for any other kind.
u8 MP_BattleServiceForKind(u8 kind);
bool8 MP_IsBattleKind(u8 kind);

#endif // GUARD_MP_BATTLE_H
