#ifndef GUARD_PLATFORM_INPUT_H
#define GUARD_PLATFORM_INPUT_H

// PS-066: key and controller remapping (src/platform/sdl2_input.c). Main thread only.
// The bindings come from pokemon-spectrum.ini next to the executable. A missing file is
// created with the defaults; a missing or unreadable line keeps that action's default.

// Controller inputs, as a bit set. The low 16 bits are the XInput wButtons bits; the rest are
// the triggers and the left stick directions, so every input can be bound the same way.
#define PAD_DPAD_UP        0x00000001
#define PAD_DPAD_DOWN      0x00000002
#define PAD_DPAD_LEFT      0x00000004
#define PAD_DPAD_RIGHT     0x00000008
#define PAD_START          0x00000010
#define PAD_BACK           0x00000020
#define PAD_LEFT_THUMB     0x00000040
#define PAD_RIGHT_THUMB    0x00000080
#define PAD_LB             0x00000100
#define PAD_RB             0x00000200
#define PAD_A              0x00001000
#define PAD_B              0x00002000
#define PAD_X              0x00004000
#define PAD_Y              0x00008000
#define PAD_LT             0x00010000
#define PAD_RT             0x00020000
#define PAD_STICK_UP       0x00040000
#define PAD_STICK_DOWN     0x00080000
#define PAD_STICK_LEFT     0x00100000
#define PAD_STICK_RIGHT    0x00200000

// Once at startup (after SDL_Init): reads or creates the config file and logs what it did.
void Platform_InputLoadConfig(void);

// Every frame: the GBA buttons held on the keyboard, and whether a fast-forward key is held.
u16 Platform_InputPollKeyboard(bool8 *fastForward);

// The GBA buttons for the controller inputs held (PAD_* bits), and whether fast-forward is held.
u16 Platform_InputMapPad(u32 padInputs, bool8 *fastForward);

#endif // GUARD_PLATFORM_INPUT_H
