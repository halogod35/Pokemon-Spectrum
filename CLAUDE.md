# CLAUDE.md — Pokémon Spectrum

Pokémon Spectrum is a native PC/Linux port of Pokémon Emerald: the pret `pokeemerald` decomp plus an SDL2
platform layer. We are adding Diablo 2-style multiplayer (each player runs the full game on their own save;
only presence is shared over the network).

**Project card (read first, its rules are binding):** `F:\Cards\Projects\pokemon-spectrum\PROJECT.md`
Status board and task cards live next to it (`BOARD.md`, `cards/PS-*.md`, `skills/SK-*.md`).

## Layout

- `src/`, `include/`, `asm/`, `data/` — decomp game code and data (shared by the GBA and PC builds)
- `src/platform/` — PC platform layer. `sdl2.c` is the entry point (`main()`) and the 60 Hz main loop
- `include/platform.h` — `Platform_*` API that game code calls into the PC layer
- `tools/` — host build tools (gbagfx, preproc, scaninc, …), built automatically by `make`
- `INSTALL_PC.md` — PC build setup (Arch, Ubuntu, Windows via WSL). `INSTALL.md` — GBA setup

## Build commands

```
make linux -j$(nproc)              # Linux, -> ./pokeemerald64
make winwsl -j$(nproc)             # Windows .exe via mingw-w64 (WSL or Arch cross), needs ./SDL2
make linux IS64BIT=0               # 32-bit variants (pokeemerald32[.exe])
make compare -j$(nproc)            # GBA matching ROM, needs agbcc in tools/agbcc; checks rom.sha1
make tidyportable                  # remove PC build output
```

The PC build defines `-D PORTABLE -D NONMATCHING -D PLATFORM_SDL2 -D UBFIX` and forces `MODERN=1`.
CI (`.github/workflows/build.yml`) runs compare, linux and winwsl on pushes to `master`/`ps-*` and PRs to `master`.

## Hard rule: the GBA build stays matching (AD-6)

- Every change to decomp files (`src/*.c`, `include/*.h`) must be inside `#ifdef PORTABLE` (or provably not
  change GBA output). New multiplayer/platform files go only into the PORTABLE build.
- `include/platform.h` is included by GBA files too (`main.c`, `save.c`, …), so PC-only declarations in it must
  be under `#ifdef PORTABLE`. The GBA compiler (agbcc) is C89-ish: no `bool`, no `<stdbool.h>`.
- If you touched `src/*.c` or `include/*.h`, run `make compare` before handoff.

```c
#ifdef PORTABLE
    MP_OnFrame();
#endif
```

## Style (follow the decomp)

- 4-space indent, braces on their own line, `CamelCase` functions.
- Types: `u8/u16/u32/s8/s16/s32/bool8/bool32` (`TRUE`/`FALSE`).
- Globals `gName`, file statics `sName`, constants `UPPER_SNAKE`.
- Multiplayer: public functions `MP_Name`, static helpers `MpName`. Game code only calls the `MP_*` API, never sockets.
- No network threads: the network is pumped once per frame on the main thread.
- Network input is untrusted: bounds/length-check everything, clamp every `memcpy`, null-terminate strings.

## Working rules

- One branch per card, `ps-XXX-slug` off `master`. Commit there. Don't push, open PRs or merge unless the user says so.
- Stay in your card's scope. Put extra work you find under "Discovered work" in the card's Handoff.
- No new system dependencies without Director approval. Vendor small libraries under `libs/`.
- Document new build steps in `INSTALL_PC.md`.
