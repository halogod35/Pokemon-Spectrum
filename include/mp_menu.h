#ifndef GUARD_MP_MENU_H
#define GUARD_MP_MENU_H

// Pokemon Spectrum multiplayer menu screens (SK-UI). Not part of the MP network layer;
// these screens talk to it only through mp/mp.h.

// Join Game: host IPv4 address + port entry screen (PS-021, src/mp/mp_menu_ip_entry.c).
// Entered from the main menu's Host/Join sub-menu. B returns there; A calls MP_Join.
void CB2_MpInitIpEntryScreen(void);

#endif // GUARD_MP_MENU_H
