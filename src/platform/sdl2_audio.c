// PS-058: PC audio output and audio measurements.
//
// The sound engine (m4a mixer + cgb_audio PSG) runs inside AudioUpdate() on the main thread and
// produces one GBA frame of audio per call: AUDIO_FRAME_SAMPLES stereo float samples at
// AUDIO_RATE Hz (sequencer included, so the music follows the audio clock, not game ticks).
// Platform_QueueAudio() receives each frame and writes it into a ring buffer. SDL's audio
// thread reads the ring in AudioCallback(); it touches nothing but the ring (AD-4: all game
// state stays on the main thread).
//
// Pacing (Platform_AudioPump, once per main loop iteration): produce GBA frames until the ring
// holds the target (--audio-queue-ms, default PLATFORM_AUDIO_DEFAULT_QUEUE_MS), at most
// AUDIO_MAX_CATCHUP frames per iteration. v0.1.0 produced at most one frame per iteration, which
// can't keep up with a 42048 Hz device unless the loop runs at 59.98 Hz or more: on a 59.94 Hz
// monitor (vsync) the queue drained and ran dry over and over. --audio-legacy restores that.
//
// If the ring does run dry (window dragged on Windows, a long stall), the callback fades the
// last sample out over AUDIO_FADE_FRAMES instead of dropping to silence, and fades the next
// sound in, so a gap is a short dip and not a click.
//
// Measurements (always collected, cheap; logged once per second with --audio-stats):
// queue level, underruns, clipped samples, large sample-to-sample jumps (also across buffer
// edges) and main loop stalls. --audio-dump <file.wav> writes the exact stream we queue
// (32-bit float WAV) so clicks can be inspected offline.

#ifdef PLATFORM_SDL2
#include <stdio.h>
#include <string.h>

#include <SDL2/SDL.h>

#include "global.h"
#include "platform.h"
#include "cgb_audio.h"
#include "platform/audio.h"
#include "platform/system.h"

#define AUDIO_RATE              42048
#define AUDIO_CHANNELS          2
#define AUDIO_DEVICE_SAMPLES    1024    // SDL device buffer, in stereo samples (24 ms)
#define AUDIO_FRAME_SAMPLES     701     // stereo samples per AudioUpdate() (one GBA frame)
#define AUDIO_BYTES_PER_SAMPLE  (AUDIO_CHANNELS * sizeof(float))
#define AUDIO_RING_FRAMES       16384   // power of two, 390 ms
#define AUDIO_MAX_CATCHUP       8       // GBA frames produced per loop iteration at most
#define AUDIO_FADE_FRAMES       64      // 1.5 ms fade out / in around a gap
#define AUDIO_LEGACY_LOW_MARK   2000    // v0.1.0: one AudioUpdate() per loop while fewer are queued

#define STATS_STALL_MS          20.0    // a main loop iteration longer than this is a stall
#define STATS_JUMP              0.5f    // a sample-to-sample step larger than this is a "jump"
#define STATS_LOG_FILE          "pokemon-spectrum_audio.txt"

struct AudioStatsWindow
{
    u32 queueMinFrames;
    u32 queueMaxFrames;
    u64 queueSumFrames;
    u32 queueSamples;
    u32 underruns;
    u32 clipped;
    u32 jumps;
    u32 edgeJumps;
    float peak;
    float maxJump;
    u32 stalls;
    double maxStallMs;
    u32 loops;
    u32 buffers;
};

// Main thread only.
static bool8 sOpen;
static bool8 sLegacy;           // --audio-legacy
static u32 sTargetFrames;       // --audio-queue-ms in stereo samples
static bool8 sStarted;          // the sound engine has produced a frame
static bool8 sEngineOn;         // the last AudioUpdate() produced a frame
static u32 sProducedFrames;     // GBA frames queued, total
static float sLast[AUDIO_CHANNELS];
static bool8 sHaveLast;
static u32 sUnderrunsSeen;
static u64 sLastPumpTicks;
static u64 sWindowStartTicks;
static u64 sOpenTicks;
static struct AudioStatsWindow sWindow;
static struct AudioStatsWindow sTotal;
static FILE *sStatsLog;
static FILE *sDump;
static u32 sDumpFrames;

// Shared with AudioCallback (SDL's audio thread): only touched with the device locked.
static SDL_AudioDeviceID sDevice;
static float sRing[AUDIO_RING_FRAMES * AUDIO_CHANNELS];
static u32 sRingRead;           // stereo samples read / written, total (wraps; only the difference counts)
static u32 sRingWrite;
static bool8 sCbProducing;      // the game is producing audio (not paused, the engine is running)
static bool8 sCbSmooth;         // fade around gaps (off with --audio-legacy)
static u32 sCbFadeIn;           // stereo samples of the fade-in done (AUDIO_FADE_FRAMES = none running)
static float sCbLast[AUDIO_CHANNELS];
static u32 sCbUnderruns;

static void StatsReset(struct AudioStatsWindow *w)
{
    memset(w, 0, sizeof(*w));
    w->queueMinFrames = 0xFFFFFFFF;
}

static void StatsMerge(struct AudioStatsWindow *total, const struct AudioStatsWindow *w)
{
    if (w->queueSamples != 0)
    {
        if (w->queueMinFrames < total->queueMinFrames)
            total->queueMinFrames = w->queueMinFrames;
        if (w->queueMaxFrames > total->queueMaxFrames)
            total->queueMaxFrames = w->queueMaxFrames;
    }
    total->queueSumFrames += w->queueSumFrames;
    total->queueSamples += w->queueSamples;
    total->underruns += w->underruns;
    total->clipped += w->clipped;
    total->jumps += w->jumps;
    total->edgeJumps += w->edgeJumps;
    if (w->peak > total->peak)
        total->peak = w->peak;
    if (w->maxJump > total->maxJump)
        total->maxJump = w->maxJump;
    total->stalls += w->stalls;
    if (w->maxStallMs > total->maxStallMs)
        total->maxStallMs = w->maxStallMs;
    total->loops += w->loops;
    total->buffers += w->buffers;
}

static double FramesToMs(u64 frames)
{
    return (double)frames * 1000.0 / AUDIO_RATE;
}

static void StatsLine(const char *line)
{
    puts(line);
    fflush(stdout);
    if (sStatsLog != NULL)
    {
        fprintf(sStatsLog, "%s\n", line);
        fflush(sStatsLog);
    }
}

static void StatsPrint(const char *prefix, const struct AudioStatsWindow *w, double seconds)
{
    char line[400];
    double avgMs = w->queueSamples ? FramesToMs(w->queueSumFrames) / w->queueSamples : 0.0;
    double minMs = w->queueSamples ? FramesToMs(w->queueMinFrames) : 0.0;
    double maxMs = w->queueSamples ? FramesToMs(w->queueMaxFrames) : 0.0;

    snprintf(line, sizeof(line),
             "[audio] %s %.1fs: queue %.0f/%.0f/%.0f ms (min/avg/max), underruns %u, clipped %u (peak %.2f), "
             "jumps>%.1f %u (at buffer edges %u, max %.2f), stalls>%.0fms %u (max %.0f ms), loops %u, buffers %u",
             prefix, seconds, minMs, avgMs, maxMs, (unsigned)w->underruns, (unsigned)w->clipped, w->peak,
             STATS_JUMP, (unsigned)w->jumps, (unsigned)w->edgeJumps, w->maxJump, STATS_STALL_MS,
             (unsigned)w->stalls, w->maxStallMs, (unsigned)w->loops, (unsigned)w->buffers);
    StatsLine(line);
}

static double TicksToSeconds(u64 ticks)
{
    return (double)ticks / (double)SDL_GetPerformanceFrequency();
}

// ---------------------------------------------------------------------------
// --audio-dump: 32-bit float stereo WAV. The header sizes are rewritten every second and at
// close, so the file is usable even if the game is killed.

static void PutLe16(u8 *p, u32 v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
}

static void PutLe32(u8 *p, u32 v)
{
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
    p[2] = (u8)(v >> 16);
    p[3] = (u8)(v >> 24);
}

#define WAV_HEADER_SIZE 58

static void DumpWriteHeader(void)
{
    u8 h[WAV_HEADER_SIZE];
    u32 dataBytes = sDumpFrames * (u32)AUDIO_BYTES_PER_SAMPLE;
    long pos;

    if (sDump == NULL)
        return;
    memcpy(h + 0, "RIFF", 4);
    PutLe32(h + 4, WAV_HEADER_SIZE - 8 + dataBytes);
    memcpy(h + 8, "WAVE", 4);
    memcpy(h + 12, "fmt ", 4);
    PutLe32(h + 16, 18);
    PutLe16(h + 20, 3);                                     // WAVE_FORMAT_IEEE_FLOAT
    PutLe16(h + 22, AUDIO_CHANNELS);
    PutLe32(h + 24, AUDIO_RATE);
    PutLe32(h + 28, AUDIO_RATE * (u32)AUDIO_BYTES_PER_SAMPLE);
    PutLe16(h + 32, (u32)AUDIO_BYTES_PER_SAMPLE);
    PutLe16(h + 34, 32);
    PutLe16(h + 36, 0);
    memcpy(h + 38, "fact", 4);
    PutLe32(h + 42, 4);
    PutLe32(h + 46, sDumpFrames);
    memcpy(h + 50, "data", 4);
    PutLe32(h + 54, dataBytes);

    pos = ftell(sDump);
    fseek(sDump, 0, SEEK_SET);
    fwrite(h, 1, sizeof(h), sDump);
    if (pos > WAV_HEADER_SIZE)
        fseek(sDump, pos, SEEK_SET);
    fflush(sDump);
}

static void DumpOpen(const char *path)
{
    sDump = fopen(path, "wb");
    if (sDump == NULL)
    {
        printf("[audio] --audio-dump: cannot write '%s'\n", path);
        return;
    }
    sDumpFrames = 0;
    DumpWriteHeader();
    printf("[audio] --audio-dump: writing the queued stream to '%s' (32-bit float, %u Hz, stereo)\n",
           path, (unsigned)AUDIO_RATE);
}

static void DumpWrite(const float *samples, u32 frames)
{
    // 0xFFFFFFFF bytes is the WAV limit: stop at about 3.4 hours.
    if (sDump == NULL || sDumpFrames > 0x7F000000u / AUDIO_BYTES_PER_SAMPLE)
        return;
    fwrite(samples, AUDIO_BYTES_PER_SAMPLE, frames, sDump);
    sDumpFrames += frames;
}

// ---------------------------------------------------------------------------
// SDL audio thread. Copies from the ring; on a gap, fades out from the last sample and fades
// the next sound in (only the ring and the sCb* variables are touched; the device is locked).

static void SDLCALL AudioCallback(void *userdata, Uint8 *stream, int len)
{
    float *out = (float *)stream;
    u32 want = (len > 0) ? (u32)len / (u32)AUDIO_BYTES_PER_SAMPLE : 0;
    u32 avail = sRingWrite - sRingRead;
    u32 n = (avail < want) ? avail : want;
    u32 i;

    (void)userdata;
    for (i = 0; i < n; i++)
    {
        u32 at = ((sRingRead + i) & (AUDIO_RING_FRAMES - 1)) * AUDIO_CHANNELS;
        float l = sRing[at];
        float r = sRing[at + 1];

        if (sCbFadeIn < AUDIO_FADE_FRAMES)
        {
            float g = (float)(sCbFadeIn + 1) / AUDIO_FADE_FRAMES;

            l *= g;
            r *= g;
            sCbFadeIn++;
        }
        out[i * AUDIO_CHANNELS] = l;
        out[i * AUDIO_CHANNELS + 1] = r;
    }
    sRingRead += n;
    if (n != 0)
    {
        sCbLast[0] = out[(n - 1) * AUDIO_CHANNELS];
        sCbLast[1] = out[(n - 1) * AUDIO_CHANNELS + 1];
    }

    if (n < want)
    {
        u32 rest = want - n;
        u32 tail = sCbSmooth ? (rest < AUDIO_FADE_FRAMES ? rest : AUDIO_FADE_FRAMES) : 0;

        // Ran dry while the game was producing: an underrun. A pause or a silent engine is not.
        if (sCbProducing)
            sCbUnderruns++;
        for (i = 0; i < rest; i++)
        {
            float g = (i < tail) ? 1.0f - (float)(i + 1) / (float)tail : 0.0f;

            out[(n + i) * AUDIO_CHANNELS] = sCbLast[0] * g;
            out[(n + i) * AUDIO_CHANNELS + 1] = sCbLast[1] * g;
        }
        sCbLast[0] = sCbLast[1] = 0.0f;
        sCbFadeIn = sCbSmooth ? 0 : AUDIO_FADE_FRAMES;
    }
}

static u32 RingLevel(void)
{
    u32 level;

    if (!sOpen)
        return 0;
    SDL_LockAudioDevice(sDevice);
    level = sRingWrite - sRingRead;
    SDL_UnlockAudioDevice(sDevice);
    return level;
}

static void RingWrite(const float *samples, u32 frames)
{
    u32 space, i;

    SDL_LockAudioDevice(sDevice);
    space = AUDIO_RING_FRAMES - (sRingWrite - sRingRead);
    if (frames > space)
        frames = space; // can't happen with the queue target; drop the excess rather than overwrite
    for (i = 0; i < frames; i++)
    {
        u32 at = ((sRingWrite + i) & (AUDIO_RING_FRAMES - 1)) * AUDIO_CHANNELS;

        sRing[at] = samples[i * AUDIO_CHANNELS];
        sRing[at + 1] = samples[i * AUDIO_CHANNELS + 1];
    }
    sRingWrite += frames;
    SDL_UnlockAudioDevice(sDevice);
}

// ---------------------------------------------------------------------------

void Platform_AudioOpen(void)
{
    SDL_AudioSpec want;

    StatsReset(&sWindow);
    StatsReset(&sTotal);
    sOpenTicks = sWindowStartTicks = SDL_GetPerformanceCounter();
    sLastPumpTicks = 0;
    sLegacy = gPlatformArgs.audioLegacy;
    sTargetFrames = (u32)((u64)gPlatformArgs.audioQueueMs * AUDIO_RATE / 1000);

    if (gPlatformArgs.audioStats)
    {
        sStatsLog = fopen(STATS_LOG_FILE, "w");
        if (sStatsLog == NULL)
            printf("[audio] --audio-stats: cannot write '%s', console only\n", STATS_LOG_FILE);
    }
    if (gPlatformArgs.audioDump[0] != '\0')
        DumpOpen(gPlatformArgs.audioDump);

    cgb_audio_init(AUDIO_RATE);

    sRingRead = sRingWrite = 0;
    sCbProducing = FALSE;
    sCbSmooth = !sLegacy;
    sCbFadeIn = AUDIO_FADE_FRAMES;
    sCbLast[0] = sCbLast[1] = 0.0f;
    sCbUnderruns = 0;

    SDL_memset(&want, 0, sizeof(want));
    want.freq = AUDIO_RATE;
    want.format = AUDIO_F32;
    want.channels = AUDIO_CHANNELS;
    want.samples = AUDIO_DEVICE_SAMPLES;
    want.callback = AudioCallback;
    // No allowed changes: SDL converts our 42048 Hz float stereo to whatever the device uses.
    sDevice = SDL_OpenAudioDevice(NULL, 0, &want, NULL, 0);
    if (sDevice == 0)
    {
        SDL_Log("Failed to open audio: %s", SDL_GetError());
        return;
    }
    sOpen = TRUE;
    SDL_PauseAudioDevice(sDevice, 0);

    if (gPlatformArgs.audioStats)
    {
        char line[300];

        snprintf(line, sizeof(line),
                 "[audio] driver '%s', %u Hz float stereo, device buffer %u samples (%.1f ms), %s, SDL %u.%u.%u",
                 SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "?", (unsigned)AUDIO_RATE,
                 (unsigned)AUDIO_DEVICE_SAMPLES, FramesToMs(AUDIO_DEVICE_SAMPLES),
                 sLegacy ? "--audio-legacy (v0.1.0 pacing, no fades)" : "queue target",
                 (unsigned)SDL_MAJOR_VERSION, (unsigned)SDL_MINOR_VERSION, (unsigned)SDL_PATCHLEVEL);
        StatsLine(line);
        if (!sLegacy)
        {
            snprintf(line, sizeof(line), "[audio] queue target %u ms (%u samples): about %u ms from sound to speaker",
                     (unsigned)gPlatformArgs.audioQueueMs, (unsigned)sTargetFrames,
                     (unsigned)(gPlatformArgs.audioQueueMs + FramesToMs(AUDIO_FRAME_SAMPLES / 2 + AUDIO_DEVICE_SAMPLES) + 0.5));
            StatsLine(line);
        }
    }
}

// Runs the sound engine for one GBA frame. FALSE if it queued nothing (sound not initialised
// yet, or the engine is switched off).
static bool8 ProduceFrame(void)
{
    u32 before = sProducedFrames;

    AudioUpdate();
    sEngineOn = (sProducedFrames != before);
    return sEngineOn;
}

void Platform_AudioPump(bool8 paused)
{
    u64 now = SDL_GetPerformanceCounter();
    bool8 producing = FALSE;

    if (sLastPumpTicks != 0)
    {
        double ms = TicksToSeconds(now - sLastPumpTicks) * 1000.0;

        if (ms > STATS_STALL_MS)
        {
            sWindow.stalls++;
            if (ms > sWindow.maxStallMs)
                sWindow.maxStallMs = ms;
        }
    }
    sLastPumpTicks = now;
    sWindow.loops++;

    if (!paused)
    {
        u32 level = RingLevel();

        if (sStarted)
        {
            if (level < sWindow.queueMinFrames)
                sWindow.queueMinFrames = level;
            if (level > sWindow.queueMaxFrames)
                sWindow.queueMaxFrames = level;
            sWindow.queueSumFrames += level;
            sWindow.queueSamples++;
        }

        if (!sOpen || sLegacy)
        {
            // v0.1.0: one GBA frame per loop iteration while less than about 48 ms is queued.
            if (level < AUDIO_LEGACY_LOW_MARK)
                ProduceFrame();
        }
        else
        {
            u32 n;

            for (n = 0; n < AUDIO_MAX_CATCHUP && level < sTargetFrames; n++)
            {
                if (!ProduceFrame())
                    break;
                level += AUDIO_FRAME_SAMPLES;
            }
        }
        producing = sEngineOn;
    }

    if (sOpen)
    {
        u32 underruns;

        SDL_LockAudioDevice(sDevice);
        sCbProducing = producing;
        underruns = sCbUnderruns;
        SDL_UnlockAudioDevice(sDevice);
        sWindow.underruns += underruns - sUnderrunsSeen;
        sUnderrunsSeen = underruns;
    }

    if (TicksToSeconds(now - sWindowStartTicks) >= 1.0)
    {
        if (gPlatformArgs.audioStats)
            StatsPrint("last", &sWindow, TicksToSeconds(now - sOpenTicks));
        StatsMerge(&sTotal, &sWindow);
        StatsReset(&sWindow);
        sWindowStartTicks = now;
        DumpWriteHeader();
    }
}

// Called by m4aSoundVSync() (music_player.c) with one GBA frame: `size` bytes of interleaved
// stereo float.
void Platform_QueueAudio(float *audioBuffer, s32 size)
{
    u32 frames;
    u32 i;

    if (audioBuffer == NULL || size <= 0)
        return;
    frames = (u32)size / (u32)AUDIO_BYTES_PER_SAMPLE;
    sStarted = TRUE;
    sProducedFrames++;

    for (i = 0; i < frames; i++)
    {
        u32 ch;

        for (ch = 0; ch < AUDIO_CHANNELS; ch++)
        {
            float s = audioBuffer[i * AUDIO_CHANNELS + ch];
            float a = s < 0.0f ? -s : s;

            if (a > 1.0f)
                sWindow.clipped++;
            if (a > sWindow.peak)
                sWindow.peak = a;
            if (sHaveLast || i != 0)
            {
                float prev = (i == 0) ? sLast[ch] : audioBuffer[(i - 1) * AUDIO_CHANNELS + ch];
                float d = s - prev;

                if (d < 0.0f)
                    d = -d;
                if (d > sWindow.maxJump)
                    sWindow.maxJump = d;
                if (d > STATS_JUMP)
                {
                    sWindow.jumps++;
                    if (i == 0)
                        sWindow.edgeJumps++;
                }
            }
        }
    }
    if (frames != 0)
    {
        sLast[0] = audioBuffer[(frames - 1) * AUDIO_CHANNELS];
        sLast[1] = audioBuffer[(frames - 1) * AUDIO_CHANNELS + 1];
        sHaveLast = TRUE;
    }
    sWindow.buffers++;

    DumpWrite(audioBuffer, frames);
    if (sOpen)
        RingWrite(audioBuffer, frames);
}

void Platform_AudioClose(void)
{
    u64 now = SDL_GetPerformanceCounter();

    StatsMerge(&sTotal, &sWindow);
    StatsReset(&sWindow);
    if (gPlatformArgs.audioStats)
    {
        char line[160];

        StatsPrint("summary", &sTotal, TicksToSeconds(now - sOpenTicks));
        // One fixed-format line for scripts (CI audio check).
        snprintf(line, sizeof(line), "[audio] result: underruns=%u clipped=%u max_stall_ms=%.0f mode=%s",
                 (unsigned)sTotal.underruns, (unsigned)sTotal.clipped, sTotal.maxStallMs, sLegacy ? "legacy" : "new");
        StatsLine(line);
    }
    if (sOpen)
        SDL_CloseAudioDevice(sDevice);
    sOpen = FALSE;
    if (sDump != NULL)
    {
        DumpWriteHeader();
        fclose(sDump);
        sDump = NULL;
    }
    if (sStatsLog != NULL)
    {
        fclose(sStatsLog);
        sStatsLog = NULL;
    }
}

void Platform_GetAudioOverlay(struct PlatformAudioOverlay *out)
{
    out->open = sOpen;
    out->queueMs = (u32)(FramesToMs(RingLevel()) + 0.5);
    out->underruns = sTotal.underruns + sWindow.underruns;
    out->clipped = sTotal.clipped + sWindow.clipped;
}

#endif // PLATFORM_SDL2
