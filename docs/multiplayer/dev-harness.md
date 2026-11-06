# Multiplayer testing: command-line flags and the dev harness

The PC build accepts command-line flags so that several copies of the game can run on one machine, each with
its own save file and its own multiplayer role, without going through menus.

> Status: the flags are **parsed and stored** in `gPlatformArgs` (`include/platform.h`). `--save`, `--title`,
> `--scale` and `--help` take effect now. `--host`, `--join`, `--port`, `--name` and `--mp-debug` are stored
> for the multiplayer session code (PS-012) and do nothing yet, apart from `--mp-debug` printing the parsed
> options at startup.

## Flags

| Flag | Meaning | Default |
|---|---|---|
| `--save <path>` | Save file to read and write. Created if it doesn't exist. | `pokeemerald.sav` (working directory) |
| `--host [port]` | Host a game. The port is optional and is only taken if the next argument starts with a digit. | port 27500 |
| `--join <ip[:port]>` | Join the game at `ip` (IPv4 address or hostname, no IPv6 literals). | port 27500 |
| `--port <n>` | Port for `--host` / `--join` (1–65535). | 27500 |
| `--name <text>` | Debug override for the displayed player name (max 31 bytes). | the save's name |
| `--mp-debug` | Verbose multiplayer/network logging. | off |
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
