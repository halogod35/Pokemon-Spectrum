#ifndef GUARD_PLATFORM_AUDIO_H
#define GUARD_PLATFORM_AUDIO_H

// PS-058: the PC audio output (src/platform/sdl2_audio.c). Main thread only.
// The game's sound engine (m4a mixer + cgb_audio PSG) is run by AudioUpdate(), one GBA frame
// (701 stereo samples at 42048 Hz) per call, and hands each frame to Platform_QueueAudio().

// After SDL_Init(SDL_INIT_AUDIO): opens the output device. Reads gPlatformArgs (--audio-*).
void Platform_AudioOpen(void);
// Once per main loop iteration: decides how many GBA audio frames to produce now, and
// collects the --audio-stats numbers. `paused` is the Ctrl+P pause.
void Platform_AudioPump(bool8 paused);
// PS-076: while on, the pump fills a deeper queue (about 200 ms) because the main loop is
// blocked by the window drag/resize loop on Windows and pumps from a timer there instead.
void Platform_AudioSetDeepQueue(bool8 on);
// Before SDL_Quit: closes the device, finishes the --audio-dump file, logs the summary.
void Platform_AudioClose(void);

// F3 overlay line (src/platform/sdl2_net_overlay.c).
struct PlatformAudioOverlay
{
    bool8 open;      // the output device is open
    u32 queueMs;     // audio waiting to be played, in ms
    u32 underruns;   // since start
    u32 clipped;     // samples past +-1.0 since start
};
void Platform_GetAudioOverlay(struct PlatformAudioOverlay *out);

#endif // GUARD_PLATFORM_AUDIO_H
