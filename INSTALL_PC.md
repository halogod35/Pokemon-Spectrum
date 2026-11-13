# Instructions

These instructions explain how to build **Pokémon Spectrum**, the PC (SDL2) port of the pokeemerald decomp, for Windows and Linux.
For the original GBA ROM build, see [INSTALL.md](INSTALL.md).

All PC builds produce an executable in the repository root:

| Command | Output |
|---|---|
| `make linux` | `pokemon-spectrum64` |
| `make linux IS64BIT=0` | `pokemon-spectrum32` |
| `make winwsl` | `pokemon-spectrum64.exe` |
| `make winwsl IS64BIT=0` | `pokemon-spectrum32.exe` |

The game reads and writes its save file, `pokemon-spectrum.sav`, in the **current working directory**.
Run the executable from the folder where you want the save to be. An older `pokeemerald.sav` (and its `.mp.cfg` / `.trade` files) in that folder is renamed to the new name once, the first time the game starts without a `pokemon-spectrum.sav`.

### Upgrading from v0.2.0 or earlier (save format 2)

Saves written by v0.2.0 and earlier are 128 KB (save format 1). This version uses a bigger save (144 KB,
save format 2: more event flags, trainer flags and vars for future regions). An old save is converted
automatically when it is loaded; the first time the game saves, it first copies the untouched old file to
`pokemon-spectrum.sav.v1.bak` (or `<your save>.v1.bak` with `--save`), so you can go back to v0.2.0 by
renaming that backup over the save. The console prints `[save] migrated ...` and `[save] backup ...` when
this happens. A save file of any other size is refused at startup (the game exits with a `[save]` message
instead of overwriting it). See `docs/save-format.md`.

Use `-j$(nproc)` to build with all CPU threads, for example `make linux -j$(nproc)`.

If you switch between `linux` and `winwsl` builds, the object folders are separate (`build/pc64`, `build/pc32`),
but the build tools in `tools/` are built for the host, so you don't need to clean between them.

### Multiplayer networking (ENet)

Multiplayer uses [ENet](http://enet.bespin.org/) 1.3.18 (MIT), vendored in `libs/enet/`, so there is nothing to
install. The Makefile compiles it into every PC build and links `-lws2_32 -lwinmm` on Windows. The build also embeds
a build id (the first 8 hex digits of the git commit, or `nogit`) that the multiplayer handshake compares. Two
games can only play together if they were built from the same commit. Override it with `make linux MP_BUILD_ID=<id>`.

Check the network layer without starting the game:

```
./pokemon-spectrum64 --mp-selftest     # prints "[MP] selftest OK" and exits 0 (FAILED / exit 1 otherwise)
```

It uses UDP ports 27590–27599 on 127.0.0.1. Windows may ask once whether to allow network access.

## Linux (Arch)

Install the required packages:

```
sudo pacman -S --needed base-devel git libpng sdl2
```

(On current Arch, `sdl2` is provided by `sdl2-compat`. That works fine.)

Build and run:

```
make linux -j$(nproc)
./pokemon-spectrum64
```

### 32-bit (optional)

Enable the `[multilib]` repository in `/etc/pacman.conf`, then:

```
sudo pacman -S --needed lib32-gcc-libs lib32-glibc lib32-sdl2
make linux IS64BIT=0 -j$(nproc)
```

### Windows executable from Arch (cross build)

You can build the Windows `.exe` directly on Arch, no Windows machine needed:

```
sudo pacman -S --needed mingw-w64-gcc
```

Then fetch the SDL2 mingw libraries and run `make winwsl` exactly as described in
[Windows: SDL2 libraries and build](#windows-sdl2-libraries-and-build) below.

## Linux (Ubuntu / Debian)

Run the following command to install the required libraries
```
sudo apt install build-essential git make libpng-dev libsdl2-dev
```

Build:

```
make linux -j$(nproc)
```

To build the 32-bit version add `IS64BIT=0` (needs `gcc-multilib` and `libsdl2-dev:i386`; the 32-bit version does not build on Ubuntu 25 and above).

You should get an executable named `pokemon-spectrum64` (or `pokemon-spectrum32`).

## Windows 10/11

The Windows build is a **cross build**: it runs inside Linux (WSL on Windows) using the mingw-w64 compiler and
produces a native `.exe`. There is no native MSVC or MSYS2 build.

### 1. Install WSL and a distro

WSL1 and WSL2 both work. In an **administrator** PowerShell:

```
wsl --install -d Ubuntu
```

On older Windows 10 builds (including LTSC 2019/2021) where `wsl --install` is not available, follow the pret WSL
steps in [INSTALL.md](INSTALL.md) instead: enable the "Windows Subsystem for Linux" optional feature, reboot, then
install a distro (for example Ubuntu) from the Microsoft Store or with `wsl --import`.

Open the distro once and create your Linux user.

Tip: the build is much faster if the repository is inside the Linux filesystem (for example `~/pokemon-spectrum`)
rather than under `/mnt/c/...` or `/mnt/f/...`, especially on WSL2.

### 2. Install the toolchain (inside WSL)

Ubuntu:

```
sudo apt update
sudo apt install build-essential git make libpng-dev g++-mingw-w64-i686 g++-mingw-w64-x86-64
```

Arch (Arch-WSL): `sudo pacman -S --needed base-devel git libpng mingw-w64-gcc`

### Windows: SDL2 libraries and build

Download the SDL2 2.0.16 mingw development libraries and extract them to the repository root as `./SDL2`
(this folder is in `.gitignore`):

```
curl -fLO https://www.libsdl.org/release/SDL2-devel-2.0.16-mingw.tar.gz
# mirror: https://archive.org/download/sdl-2-2.0.16/SDL2-devel-2.0.16-mingw.tar.gz
tar -xzf SDL2-devel-2.0.16-mingw.tar.gz
mv SDL2-2.0.16 SDL2
rm SDL2-devel-2.0.16-mingw.tar.gz
```

You should now have `SDL2/x86_64-w64-mingw32/` and `SDL2/i686-w64-mingw32/`.

Build:

```
make winwsl -j$(nproc)            # 64-bit -> pokemon-spectrum64.exe
make winwsl IS64BIT=0 -j$(nproc)  # 32-bit -> pokemon-spectrum32.exe
```

### 3. SDL2.dll

The `.exe` needs `SDL2.dll` **in the same folder as the executable**. The matching DLL is already inside the
package you extracted:

```
cp SDL2/x86_64-w64-mingw32/bin/SDL2.dll .   # for pokemon-spectrum64.exe
cp SDL2/i686-w64-mingw32/bin/SDL2.dll .     # for pokemon-spectrum32.exe
```

(Only one of them can sit next to the executables at a time, since both are called `SDL2.dll`. If you build both,
put each `.exe` and its DLL in its own folder.)

Standalone downloads also exist: [32-bit](https://archive.org/download/sdl-2-2.0.16/SDL2-2.0.16-win32-x86.zip),
[64-bit](https://archive.org/download/sdl-2-2.0.16/SDL2-2.0.16-win32-x64.zip).

### 4. Run

Double-click `pokemon-spectrum64.exe` in Explorer (a console window opens next to the game for log output), or start
it from WSL with `./pokemon-spectrum64.exe`. The save `pokemon-spectrum.sav` is created in the working directory.

## GBA

Follow instructions in [INSTALL.md](INSTALL.md). Building the matching ROM (`make compare`) needs agbcc.

## CI

`.github/workflows/build.yml` runs on pushes to `master` and `ps-*` branches and on pull requests to `master`.
It runs the GBA `make compare` build, `make linux` (64-bit, plus a non-blocking 32-bit job) and `make winwsl`
(64- and 32-bit). The Windows and Linux executables are uploaded as workflow artifacts.
