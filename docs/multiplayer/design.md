# Pokémon Spectrum — Multiplayer Design

- **Card:** PS-010 · **Status:** draft for Director approval
- **Companion docs:** `protocol.md` (wire contract), `include/mp/mp.h` (public API)
- **Binding inputs:** PROJECT.md AD-1…AD-12 and the Director rulings of 2026-11-06, including the
  user ruling that **overrides AD-6**: the GBA build no longer has to match or even build;
  multiplayer hooks in decomp files are marked with a `// MP:` comment instead of `#ifdef PORTABLE`.

## 1. Model in one paragraph

Every instance runs the full game on its own save (AD-1). One instance hosts: it is player slot 0
and also runs the server role in-process (AD-2). Clients connect only to the host over ENet/UDP
(AD-3, port 27500). The only shared state is the **player table** (who is in the session:
a 16-byte summary per player) and **presence** (map, tile, facing, movement, avatar state, busy).
The host is authoritative for the table and relays presence. All networking runs on the main
thread, pumped once per main-loop iteration (AD-4). Game code sees only `mp.h` (AD-5).

## 2. Module layout

All multiplayer code lives in `src/mp/` and `include/mp/`. Layers only call downward.

| Layer | File | Owner card | Responsibility | May include |
|---|---|---|---|---|
| API | `include/mp/mp.h` | PS-010 | The only header game code includes | `gba/types.h`, `constants/global.h` |
| features | `src/mp/mp_presence.c` | PS-030 | `MP_SetLocalPresence` change detection + 20 Hz limit + keep-alive; per-slot receive FIFO; `MP_IsValidMap` table | session, protocol |
| features | `src/mp/mp_summary.c` + `include/mp/mp_summary.h` | PS-013 | Build the summary from the save (read only), name override, ASCII debug conversion | `global.h`, save headers, wire |
| session | `src/mp/mp_session.c` | PS-011 (skeleton), PS-012 | Public `MP_*` API, state machine, event queue, player table storage, timers, dispatch, CLI auto-start, logging | client, server, transport, protocol |
| session | `src/mp/mp_server.c` | PS-012 | **Host role only**: pending peers, HELLO validation, slot/epoch assignment, relay, kick budget. No game-state access, so a headless dedicated server can reuse it later (AD-2). | protocol, transport |
| session | `src/mp/mp_client.c` | PS-012 | **Client role only**: connect timer, HELLO, WELCOME/REJECT handling, table mirror | protocol, transport |
| protocol | `src/mp/mp_protocol.c` + `include/mp/mp_protocol.h` | PS-012 (PS-011 creates it) | Message ids, sizes, **pure** parsers and writers (`protocol.md` 8.3), `MpSeqIsNewer` | wire |
| protocol | `src/mp/mp_wire.c` + `include/mp/mp_wire.h` | PS-011 | Bounded LE byte writer/reader (`MpWriteU16`, `MpReadU16`, sticky error flag) | `gba/types.h` |
| protocol | `src/mp/mp_build.c` | PS-011 | Build id string, `MP_GetBuildHash()` (FNV-1a), fake overrides. The only file compiled with `-DMP_BUILD_ID` | — |
| transport | `src/mp/mp_transport.c` + `include/mp/mp_transport.h` | PS-011 | ENet wrapper. **The only file that includes `<enet/enet.h>`.** | ENet |
| selftest | `src/mp/mp_selftest.c` | PS-011, extended by PS-012/PS-013 | `--mp-selftest`: loopback transport test + parser fuzzing | everything in `src/mp` |
| vendor | `libs/enet/**` | PS-011 | ENet 1.3.x source + `LICENSE` (MIT) | — |

Internal headers (`mp_protocol.h`, `mp_wire.h`, `mp_transport.h`, `mp_summary.h`, and an
`mp_internal.h` for shared session state and logging macros) are for `src/mp/*.c` only.
Game code including them is a review failure.

**As implemented in PS-012:**
- All session state is in a `struct MpSession` (`include/mp/mp_session_internal.h`). The game uses
  instance 0 only.
- `--mp-selftest` drives up to 7 session instances, each with a matching transport instance, plus one
  raw transport instance for hand-made packets. That lets one process run a host and several clients
  over loopback (`src/mp/mp_selftest_session.c`).
- The `--mp-fake-*` overrides are captured per session at `MP_Host`/`MP_Join`.
- ~~Presence functions are placeholders in `mp_session.c`.~~ Replaced by PS-030 (below).

**As implemented in PS-030:**
- `src/mp/mp_presence.c` (feature layer, no game state): `MP_SetLocalPresence` (sanitises and
  stores per session instance; survives `MP_Host`/`MP_Join`), send-on-change at most 20 Hz plus
  the 1 s keep-alive (`MpPresence_Update`, called by `MP_Update` after the role update), the host
  receive / relay / cache path, the client receive path, the per-slot FIFO,
  `MP_PopRemotePresence`, `MP_ClearPresenceOverflow`, `MP_IsValidMap` and the `--mp-debug`
  10 s bytes/s report. State lives in `struct MpSession.presence` (`mp_session_internal.h`).
- `src/mp/mp_presence_capture.c` (reads game state, never writes it): `MP_CaptureLocalPresence()`.
  Its only hook is one `// MP:` call at the end of the callbacks in `MainLoop()` (`src/main.c`),
  so it runs once per game tick in every callback (field, battle, menus, map loads), which is what
  the `busy` states need. It runs inside the fixed-step loop, not in `MP_Update()`.
- Field semantics: `protocol.md` 6.8.1.
- `--mp-selftest` group `src/mp/mp_selftest_presence.c` (`MpSelftest_Presence`).

### 2.1 Internal interfaces (contract for PS-011 / PS-013)

**Transport** (`mp_transport.h`, PS-011). No ENet types leak out. Peers are small integer ids.

```c
#define MP_CHANNEL_CONTROL   0
#define MP_CHANNEL_PRESENCE  1
#define MP_PEER_NONE         0xFF

enum { MP_TRANSPORT_EV_CONNECT, MP_TRANSPORT_EV_DISCONNECT, MP_TRANSPORT_EV_RECEIVE };

struct MpTransportEvent
{
    u8 type;            // MP_TRANSPORT_EV_*
    u8 peer;            // 0..peerCount-1
    u8 channel;         // RECEIVE only
    const u8 *data;     // RECEIVE only. Valid only during the callback.
    u32 length;         // RECEIVE only. Raw ENet length; NOT yet validated.
};

bool8 MpTransport_Init(void);                       // enet_initialize; no socket
void  MpTransport_Shutdown(void);                   // destroys any host, enet_deinitialize
bool8 MpTransport_Listen(u16 port, u8 maxPeers);    // server host, sets limits of protocol.md §1
bool8 MpTransport_Connect(const char *ipv4, u16 port); // client host + enet_host_connect; IPv4 dotted quad only (enet_address_set_host_ip, never DNS)
bool8 MpTransport_Send(u8 peer, u8 channel, const u8 *data, u16 len, bool8 reliable);
void  MpTransport_Poll(void (*callback)(const struct MpTransportEvent *ev)); // <= 64 events, timeout 0
void  MpTransport_Disconnect(u8 peer);              // enet_peer_disconnect_later
void  MpTransport_DisconnectNow(u8 peer);           // enet_peer_reset
void  MpTransport_Close(void);                      // destroy the ENet host (end of leave flush)
void  MpTransport_Flush(void);                      // enet_host_flush
u32   MpTransport_GetTimeMs(void);                  // enet_time_get
u16   MpTransport_GetRttMs(u8 peer);                // peer->roundTripTime (diagnostics)
// Added in PS-011:
bool8 MpTransport_IsOpen(void);                     // this instance has an ENet host
bool8 MpTransport_IsValidIPv4(const char *text);    // strict a.b.c.d check used by Connect
void  MpTransport_SelectInstance(u8 instance);      // selftest only: 2nd instance for loopback
```

As implemented in PS-011 (`include/mp/mp_transport.h` is the reference): ENet is initialised
**lazily** by the first `MpTransport_Listen`/`Connect` (or an explicit `MpTransport_Init`), not by
`MP_Init`, so single-player never calls into ENet (on Windows `enet_initialize` also calls
`timeBeginPeriod(1)`). `MP_RunSelftest()` (in `mp.h`) is the `--mp-selftest` entry point;
`gPlatformArgs.mpSelftest` selects it in `main()` before the save is read.

PS-011's card calls these `Transport_*`; they are named `MpTransport_*` here to follow the
`MP_`/`Mp` naming rule (PROJECT.md §5.5). The transport applies the ENet settings from
`protocol.md` §1 (connect magic, `maximumPacketSize`, `enet_peer_timeout`), so session code never
needs ENet.

**Summary** (`mp_summary.h`, PS-013):

```c
void  MP_BuildPlayerSummary(struct MpPlayerSummary *out);   // from gSaveBlock2Ptr + badge flags; read only
u16   MP_WritePlayerSummary(u8 *buf, u16 cap, const struct MpPlayerSummary *in); // 16, or 0 if cap < 16
bool8 MP_ReadPlayerSummary(const u8 *buf, u32 len, struct MpPlayerSummary *out); // pure; protocol.md §5.1
void  MP_NameToAscii(const u8 *gameName, char *out, u32 outSize); // logs only
```

## 3. Public API (`include/mp/mp.h`)

The header is the reference; this is the orientation.

| Group | Functions | Notes |
|---|---|---|
| Lifecycle | `MP_Init`, `MP_Update`, `MP_Shutdown`, `MP_SetDebugLogging` | `sdl2.c` only. `MP_Init` opens no socket. |
| Session | `MP_Host(port)`, `MP_Join(ip, port)`, `MP_Leave`, `MP_RefreshLocalSummary` | `port` 0 = 27500. `MP_Join` takes ASCII IPv4 only. |
| Queries | `MP_GetState`, `MP_GetErrorReason`, `MP_IsActive`, `MP_IsHost`, `MP_GetLocalSlot`, `MP_GetPlayerCount`, `MP_GetPlayer(slot)`, `MP_GetPort`, `MP_GetReasonName` | Cheap, any time. `MP_GetPlayer` returns NULL for an empty slot. |
| Events | `MP_PollEvent(&ev)` | SESSION_STARTED, CONNECT_FAILED, PLAYER_JOINED, PLAYER_LEFT, PLAYER_UPDATED, DISCONNECTED |
| Presence | `MP_SetLocalPresence`, `MP_PopRemotePresence`, `MP_ClearPresenceOverflow` | Capture calls Set every tick; avatars pop per slot. |
| Build/test | `MP_GetBuildId`, `MP_GetBuildHash`, `MP_SetFakeProtocolVersion`, `MP_SetFakeBuildId` | `protocol.md` §9 |
| Harness | `MP_SetAutoStart`, `MP_SetNameOverride`, `MP_OnContinueSavedGame`, `MP_OnNewGame` | §6 |

### 3.1 States

```
             MP_Host ok                         MP_Leave
 OFFLINE ───────────────────────────► IN_SESSION ───────► OFFLINE
   │  ▲                                  ▲   │
   │  │ MP_Leave        WELCOME          │   │ host BYE / lost / kicked (client)
   │  │                                  │   ▼
   │  └──────────── CONNECTING ──────────┘  ERROR ──MP_Leave──► OFFLINE
   │  MP_Join ok        │                     ▲    (or MP_Host / MP_Join again)
   │                    └─ REJECT / timeout ──┘
   └─ MP_Host / MP_Join fails immediately ───►ERROR
```

- `MP_IsActive()` is TRUE only in IN_SESSION. Menus (PS-022, PS-033) show LEAVE GAME / PLAYERS
  only then.
- ERROR is sticky so the UI can show `MP_GetErrorReason()`. The game keeps running single-player
  in every state.
- A host never enters ERROR because a client misbehaves; it just drops that client.

### 3.2 Events: single consumer

`MP_PollEvent` is a 32-entry ring buffer with **one** consumer: the UI layer (PS-022 owns the
polling site, and PS-033 shows the notices from it). World code (PS-031 avatars) must **not**
consume events; it reconciles with `MP_GetPlayer(slot)` every frame (slot empty → despawn;
`epoch` changed → respawn). Events carry a copy of the name, so a "X left" notice works after the
slot is freed.

### 3.3 Presence data flow

```
game tick (MainLoop hook, // MP:) ─► MP_CaptureLocalPresence ─► MP_SetLocalPresence(&p)   stores
MP_Update ─► mp_presence: changed or 1 s keep-alive, >= 50 ms since last ─► PRESENCE (ch 1)
host: validate, cache, forward to other clients
client: validate, newer-seq check ─► MpPlayer.presence + per-slot FIFO(8)
PS-031 each frame: MP_GetPlayer(slot)->presence (where), MP_PopRemotePresence (steps to replay)
```

A `stepId` gap or `presenceOverflow` means steps were missed; PS-031 catches up or snaps
(more than 2 tiles behind, or `MP_MOVE_WARP`). Remote avatars are never saved (AD-10, PS-014).

## 4. Build integration

The GBA build is no longer maintained (user ruling), so the simplest scheme is used: multiplayer
code is always part of the PC build.

### 4.1 `src/mp/*.c`: no Makefile change

`Makefile` line 335 already collects `$(C_SUBDIR)/*/*.c`, so `src/mp/*.c` is compiled with the
normal C pipeline (`cpp | preproc | cc1 | as`, the same as `src/platform/*.c`) and linked into
`pokeemerald64(.exe)` / `pokeemerald32(.exe)`. `#include "mp/mp.h"` resolves through the existing
`-iquote include`. `SUBDIRS` creates `build/pc64/src/mp/` automatically.

### 4.2 `libs/enet`: a separate plain-C rule (PS-011 adds this)

ENet must **not** go through `preproc` and our warning flags, so it gets its own rule. Proposed
addition after the "Collect sources" block (lines 334–355):

```make
# --- ENet (vendored, MIT) ---------------------------------------------------
ENET_DIR      := libs/enet
ENET_SRCS     := $(addprefix $(ENET_DIR)/,callbacks.c compress.c host.c list.c packet.c peer.c protocol.c unix.c win32.c)
ENET_OBJS     := $(patsubst %.c,$(OBJ_DIR)/%.o,$(ENET_SRCS))
ENET_CFLAGS   := -O2 -std=gnu99 -m$(BIT_WIDTH) $(LEADING_UNDERSCORE_FLAG) -I $(ENET_DIR)/include -w
ifeq ($(TARGET_OS),WINDOWS)
  OS_LFLAGS   += -lws2_32            # -lwinmm is already there (ENet uses timeGetTime)
else
  ENET_CFLAGS += -DHAS_SOCKLEN_T=1 -DHAS_POLL=1 -DHAS_FCNTL=1 -DHAS_INET_PTON=1 -DHAS_INET_NTOP=1 \
                 -DHAS_MSGHDR_FLAGS=1 -DHAS_GETADDRINFO=1 -DHAS_GETNAMEINFO=1
endif
OBJS += $(ENET_OBJS)                 # must come before SUBDIRS (line 363) so the dir is created

$(OBJ_DIR)/$(ENET_DIR)/%.o: $(ENET_DIR)/%.c
	$(MODERNCC) $(ENET_CFLAGS) -c -o $@ $<

# Only the transport sees ENet headers.
$(C_BUILDDIR)/mp/mp_transport.o: CPPFLAGS += -I $(ENET_DIR)/include
```

- `unix.c` and `win32.c` guard themselves with `#ifdef _WIN32`, so both are always listed.
- The `HAS_*` defines replace ENet's autoconf step on Linux. Without `HAS_SOCKLEN_T`, `unix.c`
  redefines `socklen_t` and fails against glibc. PS-011 must verify the exact set against the
  vendored version.
- Same compiler, `-m$(BIT_WIDTH)` and leading-underscore flag as the game, so the 32-bit mingw
  build links.
- `$(ROM): $(OBJS)` links with `$^ … $(PLATFORM_LFLAGS) $(OS_LFLAGS)`, so `-lws2_32` comes after
  the objects as it must.
- If someone still builds the GBA target, `src/mp/*.c` will not compile with agbcc. That is
  accepted under the ruling. Optional one-liner if it ever matters again:
  `C_SRCS := $(filter-out $(C_SUBDIR)/mp/%,$(C_SRCS))` under `ifneq ($(PORTABLE),1)`.

### 4.3 Build id injection (PS-011)

```make
MP_GIT_ID    := $(shell git rev-parse HEAD 2>/dev/null | cut -c1-8)
MP_BUILD_ID  ?= $(if $(MP_GIT_ID),$(MP_GIT_ID),nogit)
MP_BUILD_DIRTY := $(if $(shell git status --porcelain --untracked-files=no 2>/dev/null),1,0)
MP_BUILD_STAMP := $(OBJ_DIR)/mp_build_id.txt
# Rewrite the stamp only when the value changes, so mp_build.o rebuilds exactly then.
$(shell mkdir -p $(OBJ_DIR); echo '$(MP_BUILD_ID) $(MP_BUILD_DIRTY)' | cmp -s - $(MP_BUILD_STAMP) || echo '$(MP_BUILD_ID) $(MP_BUILD_DIRTY)' > $(MP_BUILD_STAMP))

$(C_BUILDDIR)/mp/mp_build.o: $(MP_BUILD_STAMP)
$(C_BUILDDIR)/mp/mp_build.o: CPPFLAGS += -DMP_BUILD_ID=\"$(MP_BUILD_ID)\" -DMP_BUILD_DIRTY=$(MP_BUILD_DIRTY)
```

Only `mp_build.c` sees these defines, so changing commits rebuilds one tiny file, not the game.
Hash definition and dirty/no-git policy: `protocol.md` §9.

## 5. Main loop integration (`src/platform/sdl2.c`)

All edits are marked `// MP:`. Line numbers refer to `master` at 58b0783; PS-002's changes move
them slightly.

| Where | Call | Why |
|---|---|---|
| top, includes | `#include "mp/mp.h" // MP:` | |
| `main()`, after argument parsing and before `AgbMain()` (line 131) | `MP_Init();` then the harness calls in §6 | No socket is opened. |
| `while (isRunning)` loop, **right after `ProcessEvents()` (line 144), before the `while (accumulator >= fixedTimestep)` loop** | `MP_Update();` | See below. |
| after the loop, before `CloseSaveFile()` (line 193) | `MP_Shutdown();` | sends BYE |
| `SoftReset()` (line 565, it calls `exit(0)`) | `MP_Shutdown();` before `exit` | Ctrl+R / in-game reset also says BYE |

**Why once per outer iteration, not once per `MainLoop()`.** The inner fixed-step loop runs
`MainLoop()` 0 times (paused, or a fast frame), 1 time, or several times (Space = 5× speed-up).
Pumping in the outer loop keeps networking at about display rate whatever the simulation does:
it keeps running while paused (so peers do not time out), and does not run 5× under speed-up.
This keeps AD-4's intent (main thread, once per frame, immediately before game logic) and states
the exact spot. Presence produced by several ticks in one frame is coalesced, and the latest
value wins. Receivers see a `stepId` gap and catch up.

**`MP_Update()` order** (bounded work, never blocks):

1. Auto-start pending and allowed? → `MP_Host` / `MP_Join` (§6).
2. `MpTransport_Poll` (≤ 64 events) → server or client dispatch → parse → validate → apply →
   queue events.
3. Timers: client connect timeout; host HELLO timeouts; idle timeouts; PING every 1 s.
4. Presence: send local if due; host forwards are sent from step 2.
5. Pending `PLAYER_UPDATE` (rate-limited).
6. Leave flush: after 1 s or when all peers are gone, `MpTransport_Close`.
7. `MpTransport_Flush`. Under `--mp-debug`, every 5 s, log counters (§7).

## 6. Dev harness integration (PS-002 `gPlatformArgs`)

PS-002 stores the flags in `struct PlatformArgs gPlatformArgs` (`include/platform.h`):
`savePath`, `mpRole` (`PLATFORM_MP_ROLE_NONE/HOST/CLIENT`), `port` (the last `--host <port>` /
`--port` / `--join ip:port` wins), `joinAddress`, `name` + `hasName`, `mpDebug`, `windowTitle`,
`scale`. PS-012 wires them in `main()` right after `MP_Init()`:

```c
MP_Init();                                                     // MP:
MP_SetDebugLogging(gPlatformArgs.mpDebug);                     // MP:
if (gPlatformArgs.hasName && !MP_SetNameOverride(gPlatformArgs.name))  // MP:
{
    fprintf(stderr, "--name: 1-7 characters, A-Z a-z 0-9 only\n");
    return 2;                                                  // before the window opens
}
if (gPlatformArgs.mpRole == PLATFORM_MP_ROLE_HOST)             // MP:
    MP_SetAutoStart(MP_AUTOSTART_HOST, NULL, gPlatformArgs.port);
else if (gPlatformArgs.mpRole == PLATFORM_MP_ROLE_CLIENT)      // MP:
    MP_SetAutoStart(MP_AUTOSTART_JOIN, gPlatformArgs.joinAddress, gPlatformArgs.port);
// plus --mp-fake-protocol / --mp-fake-build -> MP_SetFake*() (flags to be added to gPlatformArgs)
```

- **`--name`** (Director ruling): A–Z → `0xBB`–`0xD4`, a–z → `0xD5`–`0xEE`, 0–9 → `0xA1`–`0xAA`,
  1–7 characters, `EOS`-padded. Anything else is rejected at startup (exit status 2). The
  override only changes the network summary, never the save.
- **Auto-start trigger:** `MP_OnContinueSavedGame()` is called (`// MP:`) at the end of
  `CB2_ContinueSavedGame()` in `src/overworld.c` (after both branches, line 1774). The next
  `MP_Update()` hosts or joins. So "connect once in the overworld after Continue" (PS-012) needs
  no polling of game callbacks.
- **No save (AD-11, Director ruling):** without a save the main menu offers only NEW GAME, which
  runs `CB2_NewGame()` (line 1541). The `// MP:` hook `MP_OnNewGame()` there cancels a pending
  auto-start and prints `[MP] --host/--join needs an existing save; playing single-player`.
  The game runs normally. The same happens if a save exists but the player picks New Game
  (open question 4 in PROJECT.md: New Game into a session is not allowed).
- An auto-start that fails (port in use, bad address, rejected) logs the reason and leaves the
  game single-player. The menus (PS-020–022) use the same `MP_Host` / `MP_Join` later.
- `--mp-selftest` (PS-011) runs `mp_selftest.c` instead of the game, then exits 0/1.

## 7. Logging and counters

- Prefix `[MP]`, via `DBGPRINTF` (= `printf` in the PC build). Always logged: state changes,
  join/leave, rejects with reason names, peer drops with their malformed counters, fake overrides.
  Behind `--mp-debug`: every dropped message (type, peer, parse error), presence sent/received
  (seq, map, x, y, stepId), PING RTTs.
- `--mp-debug` counters (Director ruling), logged every 5 s: bytes/s and packets/s in and out,
  presence sent / accepted / dropped-stale / dropped-invalid, malformed per peer, `MP_Update` time
  (max and average µs). PS-031 adds object-slot and task counts in the same line format.
- Names are logged through `MP_NameToAscii`. Raw game-charset bytes are never sent to stdout.

## 8. Safety rules (AD-9, AD-10)

- Every packet goes through the pure parsers (`protocol.md` §8). Nothing touches the player
  table before full validation. Slot indices are range-checked before use.
- `src/mp` never writes a save block. `mp_summary.c` only reads `gSaveBlock2Ptr` and flags.
  Remote avatars (PS-031) are object events that must be removed before any save is written
  (PS-014 audit).
- No threads, no blocking calls: no DNS, `enet_host_service` timeout 0, bounded events per frame.
  `MP_Shutdown` may wait up to 250 ms, only at exit.

## 9. Stubbing for PS-020 / PS-021 (SK-UI)

If the UI cards start before PS-012 lands, add `src/mp/mp_stub.c` implementing every `mp.h`
function as an offline no-op: state OFFLINE, `MP_IsActive()` FALSE, `MP_GetPlayer()` NULL,
`MP_PollEvent()` FALSE, `MP_Host()`/`MP_Join()` set a fake state so screens can be exercised
(for example `MP_Join` → CONNECTING, then CONNECT_FAILED(TIMEOUT) after 5 s). PS-012 deletes the
stub in the same change that adds the real implementation. If both ever exist, the duplicate
symbols fail the link loudly, which is intended.

## 10. Who implements what

| Card | Implements from this spec |
|---|---|
| PS-011 | `libs/enet`, Makefile rules (§4.2, §4.3), `mp_transport.c`, `mp_wire.c`, `mp_build.c`, skeleton `mp_session.c` (`MP_Init/Update/Shutdown`), `sdl2.c` hooks (§5), `--mp-selftest` loopback |
| PS-012 | `mp_session.c`, `mp_server.c`, `mp_client.c`, `mp_protocol.c` (all messages except PRESENCE semantics), harness wiring (§6), parser fuzzing in the selftest, the `overworld.c` hooks |
| PS-013 | `mp_summary.c` (§2.1), summary parser and fuzz cases, `--name` conversion helper |
| PS-020/021/022 | Menus calling `MP_Host` / `MP_Join` / `MP_Leave`. PS-022 owns the `MP_PollEvent` site. |
| PS-030 | `mp_presence.c`, capture hook, PRESENCE parser semantics, `MP_IsValidMap` table |
| PS-031 | Avatars from `MP_GetPlayer` + `MP_PopRemotePresence` (localIds `0xE0 + slot` are free: the highest existing localIds are 236–240 for the Berry Blender, 127 camera and 255 player) |
| PS-033 | Notices from events, PLAYERS list from `MP_GetPlayer`. Badge count = popcount of `summary.badges`. |
