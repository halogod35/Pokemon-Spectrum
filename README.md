# Pokémon Spectrum

A native PC port of Pokémon Emerald with online multiplayer: everyone plays their own save, and you see each other in the world.

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
| **F3** | Network overlay: upload/download, ping, packet loss, audio queue |
| **Alt+Enter** or double-click | Borderless fullscreen on / off |
| Ctrl+V | Paste an IP address on the Join Game screen |
| Backspace | Delete a character on the Join Game screen |

You can resize the window and the picture scales with it. In fullscreen the picture keeps its shape, with black bars at the sides. The mouse pointer hides when the mouse is still over the game for a few seconds.

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

## New features

What this game does that a Gen 3 cartridge or emulator doesn't.

### Multiplayer: play together on single-player saves

Each player runs the whole game on their own save file, like Diablo 2. The network only shares where everyone is and what they're doing, so nothing from another player ever ends up in your save except a trade you agreed to.

- **Host or join** from **MULTIPLAYER** on the title menu, after choosing your save. The host opens UDP port 27500 and the screen shows the address friends should enter. Joining is by IP address, typed or pasted with Ctrl+V. Up to 4 players. To play over the internet, the host forwards UDP port 27500 on their router, or everyone uses a LAN or VPN.
- **See each other.** Other players walk around on your map as avatars, through doors, over ledges, during cutscenes and menus. You get "joined" and "left" notices in the field.
- **Interact.** Walk up to a player and press A: view their trainer card or Pokémon, or ask them to trade or battle. Trades and single or double battles run over a simulated link cable, several times faster than the original. Both saves commit a trade together, and a trade interrupted by a crash or disconnect is recovered.
- **Players list.** START → PLAYERS shows everyone's name, location and badges, and holds LEAVE GAME. Leaving keeps you playing offline on the same save.
- **Dropouts.** A lost connection reconnects by itself for about half a minute; the others see "lost signal" and "reconnected".
- Pause and fast-forward are switched off while you're in a trade or battle, because the other player's game runs in step with yours.
- Everyone needs the same version to play together.

### Mystery Gift

The four event tickets can be received in the game, the way the original Wireless Adapter events worked. Nothing is downloaded; the distribution runs inside the game.

1. Get the Pokédex, then answer the questionnaire at a Poké Mart counter with the phrase **LINK TOGETHER WITH ALL** and save. MYSTERY GIFT appears on the title menu.
2. MYSTERY GIFT → Wonder Cards → Wireless Communication. You receive a Wonder Card and the game saves.
3. Go upstairs in any Pokémon Center and talk to the man in green. He hands over the ticket.
4. Come back for the next card: the Eon Ticket (Southern Island, Latias or Latios), then the Aurora Ticket (Birth Island, Deoxys), the Mystic Ticket (Navel Rock, Lugia and Ho-Oh) and the Old Sea Map (Faraway Island, Mew). One card at a time, as in the original.
5. After the Hall of Fame, the ferry attendant at Lilycove Harbor takes you to the islands.

The card texts are our own; the islands, the ferry and the man's dialogue are the game's.

### Real-time clock, berries and day and night

The game follows your PC's clock, the way the battery clock in a cartridge did. The wall clock you set in your bedroom at the start is the game's time, and it starts at the PC's time, so just confirming it keeps the game in step with the real world.

- **Berries grow while the game is closed.** Plant them, come back tomorrow.
- **Daily events** happen once per real day: the Lilycove lottery, the Berry Master's wife, the shoal cave tides and the rest.
- **Day and night.** Outdoors the world is tinted by the time of day: a soft morning from 4:00, normal daylight from 10:00, an orange evening from 17:00 and a dark blue night from 20:00, blending smoothly between them. Indoors, caves, battles and menus are untouched. **DAY/NIGHT ON/OFF** in the Options menu turns it off.

### Save file

The game saves to `pokemon-spectrum.sav` next to its exe file. The same save works with the 32-bit and 64-bit builds, on Windows and Linux. Saving is crash-safe: the new save replaces the old one only once it's completely written.

## Ideas under consideration

Nothing here is decided or scheduled. These are the open ideas, with the options discussed for each.

### More Mystery Gift
The built-in distributor (see "Mystery Gift" above) is the base for these:
- **Daily lottery:** once per real day, Mystery Gift gives a random prize: Master Ball, Rare Candy, vitamins, sometimes a ticket. "Come back tomorrow!" if you try again. Needs a prize list, odds, and a rule against changing the PC clock to get extra draws.
- **Monthly events:** the distributor offers a different ticket each month, like the real events.
- **Wonder News:** use the in-game news screens for release notes or event announcements.
- **Gift folder:** drop gift files next to the game for fan events. Needs our own safe file format.
- **Gifts between friends:** pass a Wonder Card to another player online. The largest and riskiest option.

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
