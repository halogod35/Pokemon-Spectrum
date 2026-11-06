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
bool8 MpUi_TryShowFieldNotice(void);

#endif // GUARD_MP_MENU_H
