#ifndef GUARD_MAP_NAME_POPUP_H
#define GUARD_MAP_NAME_POPUP_H

// Exported type declarations
enum MapPopUp_Themes // PS-071: exported so the region table (src/region.c) can name a theme per region
{
    MAPPOPUP_THEME_WOOD,
    MAPPOPUP_THEME_MARBLE,
    MAPPOPUP_THEME_STONE,
    MAPPOPUP_THEME_BRICK,
    MAPPOPUP_THEME_UNDERWATER,
    MAPPOPUP_THEME_STONE2,
};

// Exported RAM declarations

// Exported ROM declarations
void HideMapNamePopUpWindow(void);
void ShowMapNamePopup(void);
#endif //GUARD_MAP_NAME_POPUP_H
