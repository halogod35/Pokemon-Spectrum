# CLAUDE.md — Pokémon Spectrum

Pokémon Spectrum is a native PC/Linux port of Pokémon Emerald: the pret `pokeemerald` decomp plus an SDL2
platform layer. We are adding Diablo 2-style multiplayer (each player runs the full game on their own save;
only presence is shared over the network).

**Project card (read first, its rules are binding):** `F:\Cards\Projects\pokemon-spectrum\PROJECT.md`
Status board and task cards live next to it (`BOARD.md`, `cards/PS-*.md`, `skills/SK-*.md`).

## Layout

- `src/`, `include/`, `asm/`, `data/` — decomp game code and data
- `src/platform/` — PC platform layer. `sdl2.c` is the entry point (`main()`) and the 60 Hz main loop
- `include/platform.h` — `Platform_*` API that game code calls into the PC layer
- `tools/` — host build tools (gbagfx, preproc, scaninc, …), built automatically by `make`
- `INSTALL_PC.md` — PC build setup (Arch, Ubuntu, Windows via WSL). `INSTALL.md` — upstream GBA setup (unsupported)

## Build commands

```
make linux -j$(nproc)              # Linux, -> ./pokeemerald64
make winwsl -j$(nproc)             # Windows .exe via mingw-w64 (WSL or Arch cross), needs ./SDL2
make linux IS64BIT=0               # 32-bit variants (pokeemerald32[.exe])
make tidyportable                  # remove PC build output
```

The PC build defines `-D PORTABLE -D NONMATCHING -D PLATFORM_SDL2 -D UBFIX` and forces `MODERN=1`.
CI (`.github/workflows/build.yml`) runs linux and winwsl builds on pushes to `master`/`ps-*` and PRs to `master`.

## PC only (AD-6)

- The GBA build is **unsupported** (user ruling 2026-11-06). No matching requirement, no `make compare`,
  and no `#ifdef PORTABLE` guards are required for new code. GBA targets may be removed (card PS-004).
- Mark every multiplayer hook inside an existing decomp file with a `// MP:` comment so hooks are easy to find:

```c
    MP_OnFrame(); // MP: pump presence each frame
```

## Style (follow the decomp)

- 4-space indent, braces on their own line, `CamelCase` functions.
- Types: `u8/u16/u32/s8/s16/s32/bool8/bool32` (`TRUE`/`FALSE`).
- Globals `gName`, file statics `sName`, constants `UPPER_SNAKE`.
- Multiplayer: public functions `MP_Name`, static helpers `MpName`. Game code only calls the `MP_*` API, never sockets.
- No network threads: the network is pumped once per frame on the main thread.
- Network input is untrusted: bounds/length-check everything, clamp every `memcpy`, null-terminate strings.

## Working rules

- One branch per card, `ps-XXX-slug` off `master`. Commit there. You may push your own `ps-*` branch so CI builds it (this PC has no local toolchain). Never push to `master`, open PRs or merge without the user.
- Stay in your card's scope. Put extra work you find under "Discovered work" in the card's Handoff.
- No new system dependencies without Director approval. Vendor small libraries under `libs/`.
- Document new build steps in `INSTALL_PC.md`.
