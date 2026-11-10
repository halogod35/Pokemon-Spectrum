#ifndef GUARD_PLATFORM_NET_OVERLAY_H
#define GUARD_PLATFORM_NET_OVERLAY_H

// Pokemon Spectrum multiplayer (PS-053): the F3 network overlay. For src/platform/*.c only.
// Drawn by SDL on top of the game picture (never into the GBA frame buffer), so it cannot
// change what the game draws or does.

struct SDL_Renderer;

// F3 in ProcessEvents(): show / hide.
void Platform_ToggleNetOverlay(void);
// PS-054: F4 (simulated dropout) shows the overlay so the test is visible.
void Platform_ShowNetOverlay(void);
// After SDL_RenderCopy of the game texture, before SDL_RenderPresent. Does nothing when hidden.
void Platform_DrawNetOverlay(struct SDL_Renderer *renderer);

#endif // GUARD_PLATFORM_NET_OVERLAY_H
