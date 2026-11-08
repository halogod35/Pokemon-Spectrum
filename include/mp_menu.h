#ifndef GUARD_MP_MENU_H
#define GUARD_MP_MENU_H

// Pokemon Spectrum multiplayer menu screens (SK-UI). Not part of the MP network layer;
// these screens talk to it only through mp/mp.h.

struct WindowTemplate;

// Join Game: host IPv4 address + port entry screen (PS-021/PS-022, src/mp/mp_menu_ip_entry.c).
// Entered from the main menu's Host/Join sub-menu. B returns there; A joins and shows
// "Connecting..." until the session starts (then the save continues) or fails.
void CB2_MpInitIpEntryScreen(void);

// Host Game (PS-022, src/mp/mp_menu_host.c): calls MP_Host and shows either the port and
// LAN addresses (A continues the save, B stops hosting) or why hosting failed.
// Called from the main menu's Host/Join sub-menu with the screen faded to black and the
// main menu's task and windows already freed.
void CB2_MpStartHosting(void);

// Shared helpers (src/mp/mp_ui.c).
// Resets the display to a black screen with BG0 and the given windows; frame tiles at
// 0x200 (dialog) / 0x214 (std) as LoadMessageBoxAndBorderGfx loads them.
void MpMenu_InitScreen(const struct WindowTemplate *windows);
// Fades in from black and runs `task` with the plain menu CB2/VBlank callbacks.
u8 MpMenu_StartScreen(void (*task)(u8 taskId)); // returns the task id
// ASCII digits, '.', ':', ' ' and '\n' to the game charset (anything else is dropped).
void MpMenu_AsciiToGameString(u8 *dest, const char *src, u32 destSize);
// Game-charset text (1-2 lines) for an MP_REASON_* code.
const u8 *MpUi_GetReasonText(u8 reason);
// The single MP_PollEvent consumer. Called once per frame from sdl2.c after MP_Update().
void MpUi_Update(void);
// Called from DoCB1_Overworld while field controls are unlocked. Shows a queued session
// error ("Disconnected from host", ...) when the player stands still, then MP_Leave().
// Returns TRUE if it started showing a notice (controls are then locked by it).
// It also starts the non-blocking "X joined/left the game." toast (PS-033) and returns FALSE
// for that, so the player keeps control.
bool8 MpUi_TryShowFieldNotice(void);

// PLAYERS list window in the field (PS-033), driven by the start menu: Show replaces the
// start menu window, Update redraws it twice a second, Hide removes it. Safe to call when
// not shown.
// Player interaction (PS-041, src/mp/mp_menu_interact.c).
// MP: hook in ProcessPlayerFieldInput (field_control_avatar.c): A on tile (x, y) in front of
// the player (object-event coordinates). If a remote avatar stands there, opens the player
// menu and returns TRUE (the caller then locks field controls); otherwise FALSE.
bool8 MpInteractUi_TryOpenMenu(s16 x, s16 y);
// From MpUi_TryShowFieldNotice (free field control): shows a queued incoming request
// ("NAME wants to trade. Accept?"). TRUE if it started (it locks controls itself).
bool8 MpInteractUi_TryShowIncoming(void);
// From MpUi_Update: the INTERACT events, and per-frame busy bookkeeping.
struct MpEvent;
void MpInteractUi_OnEvent(const struct MpEvent *ev);
void MpInteractUi_Update(void);

// Direct trade (PS-044, src/mp/mp_trade.c).
// The reason we cannot trade right now (a message, {STR_VAR_1} unused), or NULL: a denied map,
// already linking, fewer than two POKEMON, an ENIGMA BERRY held (the Cable Club's rules).
const u8 *MpTrade_CheckLocal(void);
// The reason `slot` cannot trade right now (a message with their name in {STR_VAR_1}), or NULL:
// gone, or in a battle / warping / not in the field / on a denied map (PS-041's busy states).
const u8 *MpTrade_CheckPartner(u8 slot);
// Both sides, once the TRADE request is ACCEPTED: save, link up, enter the trade menu. Field
// controls must be locked and the NPCs frozen (they stay so until it ends).
void MpTrade_Start(u8 partnerSlot, const u8 *partnerName);
// BUG-004: the deferred link leave after a trade, and the "group gone while trading" watchdog.
// Once per frame from MpUi_Update.
void MpTrade_Update(void);
// BUG-005: back to a plain field after a direct link activity: CB1_Overworld (player input),
// no link callback, no remote link players, link errors not suppressed.
void MpTrade_ResetFieldLinkState(void);
// PS-045 direct link battle (src/mp/mp_battle.c). `kind` is MP_INTERACT_BATTLE (single) or
// MP_INTERACT_BATTLE_DOUBLE. The checks return NULL if fine, else the text to show (it may use
// {STR_VAR_1} = the other player's name).
const u8 *MpBattle_CheckLocal(u8 kind);
const u8 *MpBattle_CheckPartner(u8 slot);
// Both players, after ACCEPTED: save, link up and start the battle. Controls stay locked.
void MpBattle_Start(u8 partnerSlot, const u8 *partnerName, u8 kind);
// Once per frame from MpUi_Update: the deferred link leave, the battle's handshake, the watchdog.
void MpBattle_Update(void);
// PS-050 trade journal: recovery messages and periodic status (every frame, from MpUi_Update),
// and the field part (roll forward + "unfinished trade" notices, from MpUi_TryShowFieldNotice;
// TRUE if it showed a message and locked controls).
void MpTrade_JournalUpdate(void);
bool8 MpTrade_TryShowJournalNotice(void);

void MpUi_ShowPlayerList(void);
void MpUi_UpdatePlayerList(void);
void MpUi_HidePlayerList(void);

#endif // GUARD_MP_MENU_H
