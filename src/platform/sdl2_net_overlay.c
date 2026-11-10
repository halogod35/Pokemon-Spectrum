// Pokemon Spectrum multiplayer (PS-053): the F3 network overlay. Shows kbit/s up and down
// (on the wire, IP/UDP headers estimated), RTT, packet loss and the player count of the
// game's session (MP_GetNetOverlay, mp.h).
//
// Drawn with SDL rectangles on top of the game texture, in the renderer's logical
// 240x160 coordinates, with a built-in 3x5 pixel font (no SDL_ttf: no new dependency).
// The GBA frame buffer is never touched. Main thread only.

#ifdef PLATFORM_SDL2
#include <stdio.h>
#include <string.h>

#include <SDL2/SDL.h>

#include "global.h"
#include "platform/net_overlay.h"
#include "mp/mp.h"
#include "platform/audio.h" // PS-058

#define OVERLAY_X           2
#define OVERLAY_Y           2
#define OVERLAY_PAD         2
#define OVERLAY_LINES       8   // 5 net + 2 simulated conditions (PS-054) + 1 audio (PS-058)
#define OVERLAY_LINE_CHARS  20
#define GLYPH_W             3
#define GLYPH_H             5
#define GLYPH_ADVANCE       (GLYPH_W + 1)
#define LINE_ADVANCE        (GLYPH_H + 2)
#define MAX_RECTS           (OVERLAY_LINES * OVERLAY_LINE_CHARS * GLYPH_W * GLYPH_H)

static bool8 sVisible = FALSE;
static SDL_Rect sRects[MAX_RECTS];

void Platform_ToggleNetOverlay(void)
{
    sVisible = !sVisible;
}

void Platform_ShowNetOverlay(void)
{
    sVisible = TRUE;
}

// 3x5 glyphs: five rows of three bits, top row in the highest octal digit, bit 2 = left.
static u16 GlyphBits(char c)
{
    static const u16 sDigits[10] = {
        075557, 026227, 071747, 071717, 055711, 074717, 074757, 071111, 075757, 075717,
    };
    static const u16 sLetters[26] = {
        025755, 065656, 034443, 065556, 074647, 074644, 034553, 055755, 072227, 011152, // A-J
        055655, 044447, 057755, 065555, 025552, 065644, 025563, 065655, 034216, 072222, // K-T
        055557, 055552, 055775, 055255, 055222, 071247,                                 // U-Z
    };

    if (c >= '0' && c <= '9')
        return sDigits[c - '0'];
    if (c >= 'a' && c <= 'z')
        c = (char)(c - 'a' + 'A');
    if (c >= 'A' && c <= 'Z')
        return sLetters[c - 'A'];
    switch (c)
    {
    case '.': return 000002;
    case ':': return 002020;
    case '/': return 011244;
    case '%': return 051245;
    case '-': return 000700;
    case '+': return 002720;
    }
    return 0;
}

// Appends the pixels of `text` at (x, y) to sRects. Returns the new rect count.
static u32 AddText(u32 count, s32 x, s32 y, const char *text)
{
    u32 i;

    for (i = 0; text[i] != '\0' && i < OVERLAY_LINE_CHARS; i++)
    {
        u16 bits = GlyphBits(text[i]);
        s32 row, col;

        for (row = 0; row < GLYPH_H; row++)
        {
            for (col = 0; col < GLYPH_W; col++)
            {
                if (!(bits & (1 << ((GLYPH_H - 1 - row) * GLYPH_W + (GLYPH_W - 1 - col)))))
                    continue;
                if (count >= MAX_RECTS)
                    return count;
                sRects[count].x = x + (s32)i * GLYPH_ADVANCE + col;
                sRects[count].y = y + row;
                sRects[count].w = 1;
                sRects[count].h = 1;
                count++;
            }
        }
    }
    return count;
}

// "12.3" kbit/s from bits per second.
static void FormatKbit(char *out, size_t size, const char *label, u32 bitsPerSec)
{
    u32 tenths = bitsPerSec / 100;

    snprintf(out, size, "%s %u.%u KBIT/S", label, (unsigned)(tenths / 10), (unsigned)(tenths % 10));
}

void Platform_DrawNetOverlay(struct SDL_Renderer *renderer)
{
    struct MpNetOverlay info;
    char lines[OVERLAY_LINES][OVERLAY_LINE_CHARS + 1];
    u32 lineCount = 0, i, count = 0, widest = 0;
    Uint8 r, g, b, a;
    SDL_BlendMode blend;
    SDL_Rect box;

    if (!sVisible || renderer == NULL)
        return;

    MP_GetNetOverlay(&info);
    memset(lines, 0, sizeof(lines));
    if (!info.open)
    {
        snprintf(lines[lineCount++], sizeof(lines[0]), "NET OFFLINE");
    }
    else
    {
        if (info.reconnecting) // PS-054
            snprintf(lines[lineCount++], sizeof(lines[0]), "NET RECONNECTING");
        else if (info.inSession)
            snprintf(lines[lineCount++], sizeof(lines[0]), "NET %s %uP", info.isHost ? "HOST" : "CLIENT", (unsigned)info.players);
        else
            snprintf(lines[lineCount++], sizeof(lines[0]), "NET CONNECTING");
        FormatKbit(lines[lineCount++], sizeof(lines[0]), "UP", info.upBitsPerSec);
        FormatKbit(lines[lineCount++], sizeof(lines[0]), "DN", info.downBitsPerSec);
        snprintf(lines[lineCount++], sizeof(lines[0]), "RTT %u+-%u MS", (unsigned)info.rttMs, (unsigned)info.rttVarMs);
        snprintf(lines[lineCount++], sizeof(lines[0]), "LOSS %u.%u%%", (unsigned)(info.lossPermille / 10), (unsigned)(info.lossPermille % 10));
    }
    // PS-054 dev tools: the simulated conditions, so a test run is never mistaken for a real one.
    if (info.simDropout && info.simDropoutLeftMs != 0)
        snprintf(lines[lineCount++], sizeof(lines[0]), "SIM DROPOUT %uS", (unsigned)((info.simDropoutLeftMs + 999) / 1000));
    else if (info.simDropout)
        snprintf(lines[lineCount++], sizeof(lines[0]), "SIM DROPOUT F4");
    if (info.simLossPercent != 0)
        snprintf(lines[lineCount++], sizeof(lines[0]), "SIM LOSS %u%%", (unsigned)info.simLossPercent);
    // PS-058: audio queued (ms) and underruns since start; "AUD OFF" if the device did not open.
    {
        struct PlatformAudioOverlay audio;

        Platform_GetAudioOverlay(&audio);
        if (!audio.open)
            snprintf(lines[lineCount++], sizeof(lines[0]), "AUD OFF");
        else
            snprintf(lines[lineCount++], sizeof(lines[0]), "AUD %uMS U%u C%u",
                     (unsigned)(audio.queueMs > 999 ? 999 : audio.queueMs),
                     (unsigned)(audio.underruns > 999 ? 999 : audio.underruns),
                     (unsigned)(audio.clipped > 999 ? 999 : audio.clipped));
    }

    for (i = 0; i < lineCount; i++)
    {
        u32 len = (u32)strlen(lines[i]);

        if (len > widest)
            widest = len;
        count = AddText(count, OVERLAY_X + OVERLAY_PAD, OVERLAY_Y + OVERLAY_PAD + (s32)i * LINE_ADVANCE, lines[i]);
    }

    SDL_GetRenderDrawColor(renderer, &r, &g, &b, &a);
    SDL_GetRenderDrawBlendMode(renderer, &blend);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    box.x = OVERLAY_X;
    box.y = OVERLAY_Y;
    box.w = (s32)(widest * GLYPH_ADVANCE) - 1 + 2 * OVERLAY_PAD;
    box.h = (s32)(lineCount * LINE_ADVANCE) - (LINE_ADVANCE - GLYPH_H) + 2 * OVERLAY_PAD;
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 176);
    SDL_RenderFillRect(renderer, &box);
    SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
    if (count != 0)
        SDL_RenderFillRects(renderer, sRects, (int)count);

    SDL_SetRenderDrawBlendMode(renderer, blend);
    SDL_SetRenderDrawColor(renderer, r, g, b, a);
}

#endif // PLATFORM_SDL2
