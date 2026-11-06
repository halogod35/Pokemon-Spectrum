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
| `--mp-fake-protocol <n>` | **Test only.** Advertise protocol version `n` (0–65535) in HELLO, so a normal host answers `REJECT(VERSION)`. | off |
| `--mp-fake-build <id>` | **Test only.** Hash `<id>` (1–15 printable ASCII) instead of the real build id, so a normal host answers `REJECT(BUILD)`. | off |
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
