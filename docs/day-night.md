# Day/night palette tinting (PS-072)

The overworld is tinted by the game clock: a soft pink dawn, vanilla colors by day, an orange
sunset and a dark blue night, the way Gen 2 did it. It is driven by the same clock as berries
and the bedroom wall clock (`RtcCalcLocalTime()` in `src/rtc.c`), it only applies outdoors, and
it can be switched off in the Options menu (**DAY/NIGHT  ON/OFF**, default ON).

- Code: `src/day_night.c`, `include/day_night.h`
- Hooks (all marked `// PS-072`): `src/palette.c` (`TransferPlttBuffer`, `BeginNormalPaletteFade`),
  `src/overworld.c` (`OverworldBasic`), `src/option_menu.c`, `include/global.h` (one options bit)

## Time bands

All times are the game clock. Each band blends linearly into the next over its last
`blendMinutes` (30) minutes, at minute resolution, so a band change is never a visible step.

| Band | Hours | Multipliers r, g, b (256 = unchanged) | Look |
|---|---|---|---|
| Morning | 04:00 – 10:00 | 246, 230, 240 | soft, slightly pink |
| Day | 10:00 – 17:00 | 256, 256, 256 | vanilla |
| Evening | 17:00 – 20:00 | 256, 198, 140 | orange sunset |
| Night | 20:00 – 04:00 | 108, 122, 200 | dark blue |

The whole thing is the `sBands[]` table at the top of `src/day_night.c`: start minute, blend
length and three channel multipliers per band. To tune a band, change its numbers; to move a
boundary, change `startMinute` (bands must stay in day order; the last one wraps past midnight).
A multiplier above 256 brightens that channel (clamped at the 5-bit maximum).

## How the tint is applied

The tint is a display-side post-process. Every frame the game copies `gPlttBufferFaded` (the
palette after weather color maps and fades) into palette memory (`PLTT`), which the renderer
reads. With the tint active, `DayNight_TintPlttBuffer()` does that copy instead and multiplies
each color's channels on the way (`out = in * mul / 256`, through a 32-entry lookup table per
channel). Nothing in `gPlttBufferUnfaded` / `gPlttBufferFaded` is ever changed, so:

- **Weather composes.** Rain, shade, drought, sandstorm, fog and the dark-cave flash all write
  their color maps into `gPlttBufferFaded` as before; the tint multiplies the result.
- **Fades reach black and white.** Black stays black because the tint only scales channels.
  For a normal palette fade toward any non-black color (white-outs, flashes) the tint strength
  shrinks with the fade coefficient (`gPaletteFade.y`), so at `y = 16` the screen is exactly
  the fade color, and a finished white-out stays white until the next fade starts.
- **Everything loaded later is tinted.** Object events (including remote players' avatars from
  `src/mp/mp_avatars.c`), door animations, reflections, field effects and weather sprites are
  ordinary palette slots and get the tint the frame they appear. No load hooks are needed.
- **Off is instant.** Switching the option off (or leaving an outdoor map) makes the next frame
  the plain copy again.

`DayNight_Update()` runs once per overworld frame (`OverworldBasic()` serves both
`CB2_Overworld` and `CB2_OverworldBasic`, which the battle transition uses). It re-reads the
RTC once a second (the game itself refreshes `gLocalTime` only every ~68 s) and recomputes
the multipliers when the minute changes.

## Where it applies

`DayNight_IsActive()` is TRUE only when all of these hold:

1. The Options toggle is ON (`gSaveBlock2Ptr->optionsDayNightOff == 0`; old saves have the bit at 0, so they are ON).
2. The player has set the clock (`FLAG_SYS_CLOCK_SET`). Before that (the moving truck and the
   first minutes in Littleroot) there is no tint.
3. The overworld is the active screen (`gMain.callback2` is `CB2_Overworld` or
   `CB2_OverworldBasic`). Battles, every menu with its own screen (bag, party, PokéNav, trainer
   card, options…), the title screen and the intro are never tinted.
4. The map is outdoors: `mapType` is `MAP_TYPE_TOWN`, `MAP_TYPE_CITY`, `MAP_TYPE_ROUTE` or
   `MAP_TYPE_OCEAN_ROUTE`. Houses, gyms, caves (`UNDERGROUND`), underwater, secret bases and
   `MAP_TYPE_NONE` are not tinted.

Map loads and warps need no special handling: the active check reads the current map header
and the first visible frame after a load is always the start of a fade.

## Exempt palettes

`sExemptPalettes` in `src/day_night.c` is a 32-bit mask of palette slots that are copied
unchanged (bit n = BG palette n for n < 16, OBJ palette n − 16 otherwise). Default:

| Slot | Used by |
|---|---|
| BG 14 | window frames (`LoadUserWindowBorderGfx`), the standard menu palette, the map name popup |
| BG 15 | the dialogue box and its text (`LoadMessageBoxGfx`) |

These are the same two slots the weather color maps leave alone (`sBasePaletteColorMapTypes`
in `src/field_weather.c`). All 16 OBJ palettes are tinted so that every sprite on the map,
local or remote, matches the tiles. The PS-053 network overlay is drawn by SDL after the frame
and is untouched.

## Known limits

- `BlendPalettes(PALETTES_ALL, 16, RGB_WHITE)`-style flashes (not palette fades) are tinted
  like everything else, so a white flash at night is slightly blue-grey for its one or two frames.
- Windows that use palette slots other than 14/15 in the field are tinted (text in them stays
  legible; it is just darker at night). Add the slot to `sExemptPalettes` if one turns out to matter.
- Nothing is sent over the network: each player sees their own clock's tint.
