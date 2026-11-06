#ifndef GUARD_MAIN_MENU_H
#define GUARD_MAIN_MENU_H

void CB2_InitMainMenu(void);
// MP: re-enter the main menu with the Host/Join sub-menu open on JOIN GAME (PS-021: B on the
// Join Game IP screen).
void CB2_ReinitMainMenuMultiplayer(void);
void CreateYesNoMenuParameterized(u8 x, u8 y, u16 baseTileNum, u16 baseBlock, u8 yesNoPalNum, u8 winPalNum);

#endif // GUARD_MAIN_MENU_H
