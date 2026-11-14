# Pokémon Spectrum

## Controls

**Game buttons**

| Key | GBA button |
|---|---|
| Arrow keys | D-pad |
| Z | A |
| X | B |
| Enter | Start |
| \ (backslash) | Select |
| **A** | **L** |
| **S** | **R** |

**Extra PC keys**

| Key | What it does |
|---|---|
| **Space** (hold) | Fast-forward, 5× speed while held |
| **Ctrl+P** | Pause / unpause |
| **Ctrl+R** | Soft reset (restart the game) |
| **F3** | Network overlay: upload/download, ping, packet loss |
| **Alt+Enter** or double-click | Borderless fullscreen on / off |
| Ctrl+V | Paste an IP address on the Join Game screen |
| Backspace | Delete a character on the Join Game screen |

You can resize the window and the picture scales with it. In fullscreen the picture keeps its shape, with black bars at the sides. The mouse pointer hides when the mouse is still over the game for a few seconds.

**Save file**

The game saves to `pokemon-spectrum.sav` next to its exe file. The same save works with the 32-bit and 64-bit builds, on Windows and Linux. Saves from versions before 0.3.1 are not supported: the game refuses them with a message and leaves the file alone, so move it away and start a new game.

**Xbox-style controller, Windows only**

| Controller | GBA button |
|---|---|
| A | A |
| **X** | B (not the controller's B button) |
| Start | Start |
| Back | Select |
| LB / RB | L / R |
| D-pad or left stick | D-pad |

**Changing the controls**

The first time the game starts, it creates `pokemon-spectrum.ini` next to the game's exe file. Open it in any text editor, change the keys or controller buttons, save, and restart the game:

```ini
[keyboard]
A           = Z, J
B           = X, K
FastForward = Space

[controller]
B           = B
FastForward = RT
```

- A button can have several keys, separated by commas. `None` leaves it unbound.
- Keyboard keys use SDL key names: `Z`, `Return`, `Space`, `Left Shift`, `Keypad 8`, `F1` and so on. The file lists more.
- Controller inputs: `A B X Y LB RB LT RT Start Back LS RS`, the D-pad (`DPadUp` …) and the left stick (`StickUp` …).
- Delete the file to get the default controls back.
- The hotkeys (Ctrl+R, Ctrl+P, F3, Alt+Enter) can't be changed.

## Ideas under consideration

Nothing here is decided or scheduled. These are the open ideas, with the options discussed for each.

### Mystery Gift and event tickets
On PC, Mystery Gift always says "The Wireless Adapter is not connected." The four event islands are already in the game and fully playable: Southern Island (Latias/Latios), Birth Island (Deoxys), Navel Rock (Lugia/Ho-Oh) and Faraway Island (Mew). Only a way to get the tickets is missing.
- **Built-in distributor (recommended base):** the game plays the part of Nintendo's distribution machine. The real Mystery Gift screens work again: you receive a Wonder Card, and the green Mystery Gift man in a Pokémon Center hands over the ticket. All four tickets, fully authentic.
- **Daily lottery:** once per real day, Mystery Gift gives a random prize: Master Ball, Rare Candy, vitamins, sometimes a ticket. "Come back tomorrow!" if you try again. Needs a prize list, odds, and a rule against changing the PC clock to get extra draws.
- **Eon Ticket only:** switch on the game's own hidden Eon Ticket hand-out. Tiny change, 100% original.
- **Monthly events:** the distributor offers a different ticket each month, like the real events.
- **Wonder News:** use the in-game news screens for release notes or event announcements.
- **Gift folder:** drop gift files next to the game for fan events. Needs our own safe file format.
- **Gifts between friends:** pass a Wonder Card to another player online. The largest and riskiest option, only useful after one of the above.
- **Simple unlock:** a menu option or command-line switch that just puts the tickets in your bag. Quick, but no ceremony.
- Any of these first needs three small fixes in the Mystery Gift code: a 64-bit address bug, a missing data stub, and a security hole that must be closed.

### Ruby/Sapphire graphics
- **Graphics pack:** an optional folder of your own Ruby/Sapphire images, chosen when the game is built. Players with either look can still play together. No Ruby/Sapphire files would be stored in this project.
- **What it would cover:** Pokémon sprites, trainers, the player, and the title screen. Map tiles, redesigned menus and the Emerald-only areas (Battle Frontier) would stay as Emerald.
- **Full swap:** about 2–3 months of work, and the result would still be a mix of both looks. Not recommended.
- **In-game toggle:** switch looks from the Options menu, realistic only for the main sprites.
- **First step:** a 1–2 day comparison of the two games' files, to answer the open questions (mainly the map tiles).

### Multiplayer
- **Faster battles:** the faster link cable is enough for now. Two bigger steps if battles ever feel slow:
  - send battle commands directly, without the simulated cable;
  - run the whole battle on the host, with players only sending their choices (like Pokémon Showdown).
- **Lost players:** fade out the avatar of a player who lost their connection. Show a "Reconnecting…" marker that stays on screen, even in menus.
- **Hosting options:** choose the port, and pick which network adapter to host on.
- **More players:** only send each player the positions of players near them, if the player limit ever grows.

### Game and PC polish
- **Graphics polish:** small visual issues spotted in play-tests, still to be listed and looked at.
- **Soft reset without closing the window:** reset the game in place instead of restarting the program.
- **Sound:**
  - a limiter if loud battles with Pokémon cries ever distort;
  - a filter if low "thumps" are heard at the end of notes.
- **Console window:** clicking inside the game's console window freezes the game while text is selected there (a Windows console quirk). Turn that selection mode off at startup, or log to a file instead.
- **Controllers on older or clean Windows:** start even when `XINPUT1_3.dll` is missing (Windows XP, some fresh installs), with controllers simply turned off.

### Developer housekeeping
- A debug build with memory-error checking (sanitizers).
- Remove the unused GBA build targets.
