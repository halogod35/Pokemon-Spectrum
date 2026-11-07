# Multiplayer testing: command-line flags and the dev harness

The PC build accepts command-line flags so that several copies of the game can run on one machine, each with
its own save file and its own multiplayer role, without going through menus.

> Status (PS-012): `--host` / `--join` start a session automatically once **Continue** reaches the overworld.
> Choosing **New Game** (or having no save, which only offers New Game) cancels them with
> `[MP] --host/--join needs an existing save` and the game runs single-player (AD-11). If the session can't
> start (port in use, bad address, rejected), the reason is logged and the game keeps running single-player.
> The menus (PS-020–022) use the same session code later.

## Flags

| Flag | Meaning | Default |
|---|---|---|
| `--save <path>` | Save file to read and write. Created if it doesn't exist. | `pokeemerald.sav` (working directory) |
| `--host [port]` | Host a game. The port is optional and is only taken if the next argument starts with a digit. | port 27500 |
| `--join <ip[:port]>` | Join the game at `ip`. **IPv4 dotted quad only** (`a.b.c.d`). A hostname is refused when connecting (`BAD_ADDRESS`), because a DNS lookup would block the game loop (AD-4). | port 27500 |
| `--port <n>` | Port for `--host` / `--join` (1–65535). | 27500 |
| `--name <text>` | Debug override for the name other players see (never saved). 1–7 characters, `A-Z a-z 0-9` only. Anything else exits with code 2 before the window opens. | the save's name |
| `--mp-debug` | Verbose multiplayer/network logging. | off |
| `--mp-selftest` | Run the multiplayer self-test (loopback on 127.0.0.1, UDP 27590–27599) without a window or a save, print `[MP] selftest OK` / `FAILED`, and exit 0 / 1. Added by PS-011. | off |
| `--mp-link-test` | **Test only** (PS-046). With `--host` / `--join`: once slots 0 and 1 are in the session and in the overworld, both form a link group, `OpenLink()`, run the player-data exchange and 3 × 200-byte `SendBlock` rounds, close, and repeat every second. Each step is logged as `[MP] link test: ...`. Kill one instance and the other logs `link-error path reached` and shows the game's link error screen (recovery is PS-048; close the window). | off |
| `--mp-fake-protocol <n>` | **Test only.** Advertise protocol version `n` (0–65535) in HELLO, so a normal host answers `REJECT(VERSION)`. | off |
| `--mp-fake-build <id>` | **Test only.** Hash `<id>` (1–15 printable ASCII) instead of the real build id, so a normal host answers `REJECT(BUILD)`. | off |
| `--debug-slow-save` | **Test only.** Pause 50 ms after each 4 KB sector while the save file is written (1.6 s per file write; one in-game save writes the file twice, so about 3.2 s), so the process can be killed mid-save. Added by PS-014. | off |
| `--save-selftest` | Check the crash-safe save I/O on `<save>.selftest` (never the real save) without a window, print `[save] selftest OK` / `FAILED`, and exit 0 / 1. Added by PS-014. | off |
| `--title <text>` | Window title (max 127 bytes). | `Pokémon Spectrum`, `Pokémon Spectrum [HOST]` or `Pokémon Spectrum [CLIENT]` |
| `--scale <n>` | Initial window scale, 1–10. | 1 |
| `-h`, `--help` | Print help and exit. | |

Rules:

- `--host` and `--join` can't be used together.
- If a port is given more than once (`--host 27501`, `--join 1.2.3.4:27502`, `--port 27503`), the last one wins.
- An unknown flag, a missing value or an invalid value prints an error plus the help text and exits with code 2.
  `--help` exits with code 0.
- With no flags the game behaves as before: it uses `pokeemerald.sav` in the working directory. The only
  difference is the window title (`Pokémon Spectrum` instead of `pokeemerald`).

On Windows the help and errors appear in the console window that opens next to the game. If you double-click
the `.exe` with bad flags (for example from a shortcut), that window closes immediately, so run it from `cmd`
to read the message.

## Running a host + client pair

Build first (`make linux` or `make winwsl`, see `INSTALL_PC.md`), then from the repository root:

```
tools/mp/run_pair.sh            # Linux
tools\mp\run_pair.bat           # Windows (SDL2.dll next to pokeemerald64.exe)
```

Each script starts:

```
pokeemerald64 --save host.sav   --host 27500           --mp-debug
pokeemerald64 --save client.sav --join 127.0.0.1:27500 --mp-debug
```

Any extra arguments are passed to both instances, for example `tools/mp/run_pair.sh --scale 2`.

Overrides (environment variables): `EXE` (path to the executable), `SAVE_DIR` (where `host.sav` and
`client.sav` live, default the repository root) and `PORT` (default 27500).

If `host.sav` or `client.sav` don't exist yet and `pokeemerald.sav` exists in the repository root, the scripts
copy it to create them. Existing saves are never overwritten. Multiplayer requires an existing save (AD-11),
so start each instance once and save in-game, or copy a save you already have.

You can also run the instances by hand, for example a third player:

```
./pokeemerald64 --save p3.sav --join 127.0.0.1 --title "P3"
```

## Checking that saves are independent

1. `./pokeemerald64 --save a.sav` and, at the same time, `./pokeemerald64 --save b.sav`.
2. In instance A, move somewhere and save. Close both.
3. Start `--save b.sav` again: B continues from where B was saved, not from A's position.
   `a.sav` and `b.sav` have different contents (`cmp a.sav b.sav`).

## Save file safety (PS-014)

The PC build writes the save crash-safely: the whole 128 KB image goes to `<save>.tmp`,
which is flushed and synced to disk, and then renamed over `<save>` (`rename()` on Linux,
`MoveFileExA(..., MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)` on Windows). If the
game dies mid-save, `<save>` is still the previous complete save. A leftover `<save>.tmp`
is harmless: it is never loaded, and the next save overwrites it. If a write fails, the
game logs `[save] ...` on stderr and the previous save stays as it was.

`--save-selftest` checks this automatically (CI runs it on Linux).

### Manual kill-during-save test (repeat about 20 times; Windows and Linux)

1. Make a known-good save: `./pokeemerald64 --save kill.sav`, play until you can save, save,
   and quit. Copy it: `cp kill.sav kill-good.sav`.
2. Start `./pokeemerald64 --save kill.sav --debug-slow-save`, continue, and walk a few
   steps so the new save differs from the old one.
3. Open START > SAVE and confirm. The console prints
   `[save] --debug-slow-save: writing 'kill.sav' over 1600 ms` (twice: the game writes the file
   twice per save, and each write is atomic on its own).
4. Within those 3.2 s, kill the process: Linux `kill -9 $(pgrep pokeemerald64)`, Windows
   Task Manager > End task (or `taskkill /F /IM pokeemerald64.exe`). Vary the moment:
   immediately, about halfway, near the end.
5. Start `./pokeemerald64 --save kill.sav` again. Expected: the main menu shows CONTINUE
   with no "save file is corrupted" message, and CONTINUE loads either the old position or
   the new one (if the kill landed after the rename). `kill.sav` is exactly 131072 bytes.
6. A `kill.sav.tmp` may exist after step 4. That is expected. It disappears after the next
   successful save.

Pass: all runs load. Any "corrupted" or "deleted" save message, or a missing or short
`kill.sav`, is a Sev-1 failure.

### Saving in a session

Saving in a multiplayer session works the same as in single-player: START > SAVE writes your
own save (AD-10). Remote data is never written. To check: host and join with two saves, save
in both while the players can see each other, quit, start each save offline
(no `--host` / `--join`), and CONTINUE. Each game loads its own position, and there is no
extra player or NPC where the other player stood.

## Virtual link cable test (PS-046)

```
pokeemerald64 --save host.sav   --host 27500           --mp-link-test --mp-debug
pokeemerald64 --save client.sav --join 127.0.0.1:27500 --mp-link-test --mp-debug
```

Continue on both. Expected on both consoles, repeating: `grouped as link id N`, `2 players on the cable`
(master), two `gLinkPlayers[i] = ...` lines naming both saves (the same on both consoles), `round 1..3: 200-byte blocks exchanged and
verified`, `cycle N complete`. The game keeps running (walk around); the link runs alongside the overworld.

- **Disconnect:** close one window (BYE, immediate) or kill the process (detected within the 10 s idle timeout). The survivor logs `link group ... ended: LOST`
  (or `SESSION`) and `link-error path reached (CB2_LinkError ...)` and shows the link error screen.
- **Latency (card acceptance, 150 ms):** Linux `sudo tc qdisc add dev lo root netem delay 75ms` (75 ms each
  way on loopback = 150 ms RTT; remove with `sudo tc qdisc del dev lo root`). Windows: clumsy, "Lag" 150 ms on
  the loopback filter. Rounds take longer (each block is about 15 rows plus round trips); they must still
  verify.
