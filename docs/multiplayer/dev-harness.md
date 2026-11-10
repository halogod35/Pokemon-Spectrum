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
| `--save <path>` | Save file to read and write. Created if it doesn't exist. | `pokemon-spectrum.sav` (working directory) |
| `--host [port]` | Host a game. The port is optional and is only taken if the next argument starts with a digit. | port 27500 |
| `--join <ip[:port]>` | Join the game at `ip`. **IPv4 dotted quad only** (`a.b.c.d`). A hostname is refused when connecting (`BAD_ADDRESS`), because a DNS lookup would block the game loop (AD-4). | port 27500 |
| `--port <n>` | Port for `--host` / `--join` (1–65535). | 27500 |
| `--name <text>` | Debug override for the name other players see (never saved). 1–7 characters, `A-Z a-z 0-9` only. Anything else exits with code 2 before the window opens. | the save's name |
| `--mp-debug` | Verbose multiplayer/network logging. | off |
| `--mp-net-stats` | Network traffic log (PS-053): a summary every 10 s and a per-message-type table when the session ends. See "Network statistics" below. **F3** toggles the in-game overlay with or without this flag. | off |
| `--mp-selftest` | Run the multiplayer self-test (loopback on 127.0.0.1, UDP 27590–27599) without a window or a save, print `[MP] selftest OK` / `FAILED`, and exit 0 / 1. Added by PS-011. | off |
| `--mp-link-test` | **Test only** (PS-046). With `--host` / `--join`: once slots 0 and 1 are in the session and in the overworld, both form a link group, `OpenLink()`, run the player-data exchange and 3 × 200-byte `SendBlock` rounds, close, and repeat every second. Each step is logged as `[MP] link test: ...`. Kill one instance and the other logs `link-error path reached` and shows the game's link error screen (recovery is PS-048; close the window). | off |
| `--mp-link-stats` | PS-052: log link cable measurements (rows, blocks, waits) for each link activity. See "Fast cable measurements" below. | off |
| `--mp-link-rows` | **Test only** (PS-052). Send link blocks row by row like the GBA cable (no `LINK_BLOCK`), to compare. | off |
| `--mp-net-loss <pct>` | **Test only** (PS-054). Drop `pct` % (0–100) of this game's UDP datagrams at random, both directions. See "Faking a bad connection" below. | 0 |
| `--mp-net-dropout <s>` | **Test only** (PS-054). Length of the simulated signal loss that **F4** starts (0–600 s; 0 = until F4 is pressed again). F4 works without this flag. | 0 |
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
- With no flags the game behaves as before: it uses `pokemon-spectrum.sav` in the working directory. The only
  difference is the window title (`Pokémon Spectrum` instead of `pokemon-spectrum`).

On Windows the help and errors appear in the console window that opens next to the game. If you double-click
the `.exe` with bad flags (for example from a shortcut), that window closes immediately, so run it from `cmd`
to read the message.

## Running a host + client pair

Build first (`make linux` or `make winwsl`, see `INSTALL_PC.md`), then from the repository root:

```
tools/mp/run_pair.sh            # Linux
tools\mp\run_pair.bat           # Windows (SDL2.dll next to pokemon-spectrum64.exe)
```

Each script starts:

```
pokemon-spectrum64 --save host.sav   --host 27500           --mp-debug
pokemon-spectrum64 --save client.sav --join 127.0.0.1:27500 --mp-debug
```

Any extra arguments are passed to both instances, for example `tools/mp/run_pair.sh --scale 2`.

Overrides (environment variables): `EXE` (path to the executable), `SAVE_DIR` (where `host.sav` and
`client.sav` live, default the repository root) and `PORT` (default 27500).

If `host.sav` or `client.sav` don't exist yet and `pokemon-spectrum.sav` exists in the repository root, the scripts
copy it to create them. Existing saves are never overwritten. Multiplayer requires an existing save (AD-11),
so start each instance once and save in-game, or copy a save you already have.

You can also run the instances by hand, for example a third player:

```
./pokemon-spectrum64 --save p3.sav --join 127.0.0.1 --title "P3"
```

## Checking that saves are independent

1. `./pokemon-spectrum64 --save a.sav` and, at the same time, `./pokemon-spectrum64 --save b.sav`.
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

1. Make a known-good save: `./pokemon-spectrum64 --save kill.sav`, play until you can save, save,
   and quit. Copy it: `cp kill.sav kill-good.sav`.
2. Start `./pokemon-spectrum64 --save kill.sav --debug-slow-save`, continue, and walk a few
   steps so the new save differs from the old one.
3. Open START > SAVE and confirm. The console prints
   `[save] --debug-slow-save: writing 'kill.sav' over 1600 ms` (twice: the game writes the file
   twice per save, and each write is atomic on its own).
4. Within those 3.2 s, kill the process: Linux `kill -9 $(pgrep pokemon-spectrum64)`, Windows
   Task Manager > End task (or `taskkill /F /IM pokemon-spectrum64.exe`). Vary the moment:
   immediately, about halfway, near the end.
5. Start `./pokemon-spectrum64 --save kill.sav` again. Expected: the main menu shows CONTINUE
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
pokemon-spectrum64 --save host.sav   --host 27500           --mp-link-test --mp-debug
pokemon-spectrum64 --save client.sav --join 127.0.0.1:27500 --mp-link-test --mp-debug
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

## Log file and link flight recorder (PS-051)

Every `[MP]` console line also goes to a log file in the working directory (the folder the
game is started from), flushed line by line, so the file is complete even if the game is killed.
The game prints the full path at startup: `[MP] log file: ...`.

- `mp_log.txt`: the normal name. It is overwritten at every start, so copy it before you
  start the game again.
- `mp_log_<pid>.txt`: used if another running instance (a second window on the same PC)
  already holds `mp_log.txt`. Then the two logs do not overwrite each other.

The **link flight recorder** keeps the last 600 ticks (10 s) of link state while a trade or a
battle link is in use. On an incident it writes them into the log file, under a
`[MP] ==== link trace (reason) ====` header. An incident is `CB2_LinkError` (the communication
error screen), rejected link data, a cable hardware error, or a link error that
`TrySetLinkErrorBuffer` sees, suppressed or not. There is one dump per incident: triggers within
2 s count as the same incident and get a one-line `same incident, also: ...`. A
`link trace aftermath` block follows with the ticks after the dump. Columns: `gMain.callback2`
and `callback1`, and `gLinkCallback` as `exe+offset`. To resolve an offset, look it up in the
`pokemon-spectrum.map` shipped with the build: offset + 0x140000000 (Win64) or + 0x400000 (Win32).
On Linux, use `nm` on the same binary. Then come the link flags, the MP link state, the cable,
the trade menu's `callbackId` (255 = no menu) and the first word of the command sent and the row
received. The dump header lists the offsets of `CB2_LinkError`, `CB2_Overworld`,
`CB2_ReturnToField` and `CB1_Overworld` as a cross-check.

**Play-test bug reports:** send `mp_log*.txt` from **both** machines (and
`pokemon-spectrum_crash.txt` if there is one).

## Network statistics (PS-053)

Every packet the game hands to ENet, and every packet ENet delivers, is counted per message type
and per peer (`src/mp/mp_netstats.c`, a few additions per packet, always on). Nothing about the
traffic changes. Three ways to look at it:

- **F3 overlay** (any time): a small box at the top left of the game picture with
  `NET HOST 3P` / `NET CLIENT 3P`, `UP` and `DN` in kbit/s, `RTT` (ms, +- variance) and `LOSS`.
  It shows the worst connected peer, so on a host that is the slowest client. The rates are over
  the last second and count what goes over the network: ENet's UDP payload plus an estimated
  28 bytes of IPv4 + UDP header per datagram. The overlay is drawn by SDL on top of the picture;
  it never touches the game's frame buffer.
- **`--mp-net-stats`**: every 10 s a `[MP] netstats: last 10.0 s: ...` line with up and down
  rates (our message bytes, ENet bytes, kbit/s with IP/UDP), the top three message types by bytes,
  and RTT and loss per peer. When the session's transport closes (leave, error, quit) a full table
  follows: count and bytes sent and received per message type with each type's share, totals,
  ENet's own wire totals, per-peer totals, RTT and loss.
- **`--mp-selftest`** runs the traffic benchmark (PS-053) at the end: a host + 1 client and a
  host + 3 clients over loopback at 60 Hz, measuring idle, walking, mach bike, a trainer card +
  party view, a trade and a link battle. Look for the `[MP] netbench:` lines in the output (the CI
  log of the "Multiplayer self-test" step): one line per instance per activity, a line per message
  type, and at the end the summary table. The trade and battle rows run the real session and link
  bus with the command stream of the game's link screens (block sizes from `trade.c` and
  `battle_main.c`); the menu waits and the per-turn battle traffic are estimates.

Terms: **msg** = our packets (4-byte header + payload); **ENet** = UDP payload (msg plus ENet's
command headers, acknowledgements and its own pings); **IP** = ENet + 28 bytes per datagram.
ENet's RTT and loss come from the peer (`roundTripTime`, `roundTripTimeVariance`, `packetLoss`;
loss is measured on reliable packets over 10 s windows).

## Fast cable measurements (PS-052)

Since PS-052 a `SendBlock` of up to 500 bytes crosses the virtual cable as one message
(`LINK_BLOCK`, protocol.md 6.12 "Whole blocks") instead of one 14-byte row per tick. Two flags
help to compare and to measure on real machines:

| Flag | Effect | Default |
|---|---|---|
| `--mp-link-stats` | Logs one `[MP] link stats (...)` line when each link activity ends (the cable closes) and every 10 s while linked: ticks on the cable, rows played (and how many by the catch-up), blocks sent and received (whole / as rows), the average ticks until our own block came back from the bus, ticks with nothing received (and how many of those were waiting for our own data to come back), and ticks a block waited for the game to take the previous one. | off |
| `--mp-link-rows` | **Test only.** Send every block row by row like the GBA cable (the pre-PS-052 behaviour), to compare. Receiving works the same, so only one side needs it. | off |

To compare a battle: play the same battle once with `--mp-link-stats` on both, once with
`--mp-link-stats --mp-link-rows` on both, and compare the `link stats` lines (and the feel).
The flight recorder shows a tick that delivered a whole block as `rxB=<link id>,<size>`, and a tick
that sent one as `tx=FB0C`.

## Faking a bad connection (PS-054)

A session survives a signal loss (protocol.md 7.6): about 2 s goes unnoticed, a longer one makes
the client reconnect by itself (same slot, "Reconnecting..." / "Reconnected!" toasts), and only
after about 30–35 s is the player dropped as before. To try it on one PC without unplugging
anything, the game can drop its own network traffic:

- **F4** cuts this game's network completely (nothing sent or received), as when a phone loses its
  signal. Press F4 again to bring it back, or start the game with `--mp-net-dropout <s>` and F4
  cuts it for exactly `s` seconds. F4 also opens the F3 overlay, which shows `SIM DROPOUT` (with
  the seconds left), `NET RECONNECTING` while the client retries, and the real RTT / loss.
  Alt+F4 is not affected (it still closes the window).
- **`--mp-net-loss <pct>`** drops that share of datagrams at random for the whole run (overlay:
  `SIM LOSS`). 5–20 % is a bad mobile link; ENet resends what matters, so the game should just
  feel laggier.

The simulation is per game instance and both directions, so press F4 on the **client** to test the
client's signal loss (the host sees the same thing a real loss would show it). Every start and end
is logged (`[MP] net sim: ...`) in the console and `mp_log.txt`.

### Recipe (two instances on one PC)

```
pokemon-spectrum64 --save a.sav --host --title HOST
pokemon-spectrum64 --save b.sav --join 127.0.0.1 --title CLIENT --mp-net-dropout 10
```

Continue both saves and meet on one map, then press F4 in the CLIENT window:

| Dropout | What you should see |
|---|---|
| 2 s (`--mp-net-dropout 2`) | Both players freeze for a moment, then carry on. No notice anywhere. |
| 10 s | After 4–6 s the client shows "Reconnecting..." and the host "NAME lost signal..."; the client's avatar stands still on the host. When the 10 s are over the client reconnects within a few seconds: "Reconnected!" / "NAME reconnected.", same slot, both keep walking. PLAYERS shows the client as `LOST` while it is away. |
| 40 s | As 10 s, but the client gives up after its 30 s window ("The connection to the host was lost. You are now playing offline.") and the host shows "NAME left the game." after its 35 s grace. |
| during a trade or battle | A loss longer than about 4 s ends it like a disconnect does today: the PS-048 link error screen and "Continue from last save". A trade that was already committed by the host is finished on the dropped side by the PS-050 journal after the reconnect ("trade completed"), otherwise it is cancelled on both. Shorter losses only stall the trade or battle. |
