#ifndef GUARD_PLATFORM_TEXT_INPUT_H
#define GUARD_PLATFORM_TEXT_INPUT_H

// Pokemon Spectrum multiplayer (PS-021): SDL side of the keyboard text entry declared in
// platform.h. For src/platform/*.c only (game code uses the Platform_* functions in platform.h).

union SDL_Event;

// Called by ProcessEvents() in sdl2.c for every SDL event. Does nothing unless
// Platform_StartTextInput() is active. Never consumes the event.
void Platform_HandleTextInputEvent(const union SDL_Event *event);

#endif // GUARD_PLATFORM_TEXT_INPUT_H
