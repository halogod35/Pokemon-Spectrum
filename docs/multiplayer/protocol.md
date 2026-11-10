# Pokémon Spectrum — Multiplayer Wire Protocol

- **Card:** PS-010 (spec), implemented by PS-011 (transport), PS-012 (session), PS-013 (summary), PS-030 (presence), PS-041 (interaction requests), PS-046 (virtual link cable), PS-042 (trainer card), PS-043 (view party)
- **Protocol version:** `MP_PROTOCOL_VERSION = 11` (2 = `INTERACT_*`, PS-041; 3 = `LINK_*`, PS-046; 4 = `TRAINER_CARD_DATA`, PS-042; 5 = `PARTY_DATA`, PS-043; 6 = `TRADE_COMMIT`, PS-044; 7 = `TRADE_RECOVER`, PS-050; 8 = INTERACT kind BATTLE_DOUBLE, PS-045; 9 = `LINK_DONE`, PS-051; 10 = `LINK_BLOCK`, PS-052; 11 = reconnect token, `RESUME`, `PLAYER_STATUS`, PS-054; see section 11)
- **Status:** draft for Director approval
- **Companion docs:** `design.md` (modules, API, build), `include/mp/mp.h` (public API)

This document is the contract. If code and this document disagree, the code is wrong or this
document must be updated in the same change **and** `MP_PROTOCOL_VERSION` bumped.
Any change to any byte layout, message type number, channel assignment or validation rule that
changes what a peer accepts is a wire change and **must** bump `MP_PROTOCOL_VERSION`.

---

## 1. Transport

| Item | Value |
|---|---|
| Library | ENet 1.3.x, vendored in `libs/enet/` (AD-3) |
| Address family | IPv4 only (ENet 1.3 limitation). Host binds `ENET_HOST_ANY` (0.0.0.0). |
| Default port | **27500/UDP** (`MP_DEFAULT_PORT`), configurable |
| Topology | Star. Clients connect **only** to the host. The host relays (AD-2). |
| ENet channel count | **2** |
| ENet connect `data` | `0x504D5350` (`MP_CONNECT_MAGIC`). The host immediately resets (`enet_peer_reset`) any incoming ENet connection whose connect data differs, without sending anything. |
| Host peer count | `MP_MAX_PLAYERS + 2` (= 6, PS-054). 3 for clients + **1 spare**, so a 5th player can connect at the ENet level and receive `REJECT(FULL)` instead of a silent ENet refusal, + 2 so a reconnecting client (7.6) can connect while the host still holds its stale peer. |
| Client peer count | 1 |
| Bandwidth limits | 0 / 0 (unlimited; our traffic is tiny) |
| Compression | none |
| `host->maximumPacketSize` | `MP_MAX_PACKET_SIZE` = **512** (ENet ≥ 1.3.13 has this field; it defaults to 32 MB, which is a memory DoS) |
| `host->maximumWaitingData` | 16384 |
| `enet_peer_timeout(peer, 0, 4000, 7000)`, `enet_peer_ping_interval(peer, 500)` | Set on every peer when its ENet connection is established (a client: also when it dials). PS-054: was 5000 / 10000. A loss of about 2 s is ridden out; a longer one is noticed after 4–7 s and handed to the reconnect (7.6). |
| Service | `enet_host_service(host, &ev, 0)` in a loop, on the main thread, once per `MP_Update()`, **at most 64 events per call** (bounds frame time under a flood; the rest are handled next frame) |

### 1.1 Channels

| Channel | ENet flags | Used for |
|---|---|---|
| **0** `MP_CHANNEL_CONTROL` | `ENET_PACKET_FLAG_RELIABLE` | All session/control messages (`0x01`–`0x07`), `INTERACT_*` (6.11) and the virtual link cable (`0x60`–`0x6B`, 6.12) and `TRADE_COMMIT` (`0x6C`, 6.15) |
| **1** `MP_CHANNEL_PRESENCE` | `0` (unreliable, **sequenced** by ENet per channel) | `PRESENCE`, `PING`, `PONG` |

A message received on the wrong channel is dropped (counts as malformed, see 8.2).

### 1.2 One message per packet

Each ENet packet carries **exactly one** message: a 4-byte header plus its payload. There is no
batching in protocol version 1. Trailing bytes after the payload are a validation failure.

---

## 2. Encoding rules

- **Little-endian** for every multi-byte integer.
- **No raw structs on the wire.** Every field is written and read individually with the
  `MpWriteU8/U16/U32/S16` and `MpReadU8/U16/U32/S16` helpers (`mp_wire.c`). In-memory structs
  (`struct MpPlayerSummary`, `struct MpPresence`, …) are *not* the wire layout and may be padded
  differently on 32-bit and 64-bit builds. 32-bit and 64-bit builds, Windows and Linux, must
  interoperate.
- Signed integers (`s16`) are two's complement.
- Strings: player names are in the **game charset** (`charmap.txt`), `EOS = 0xFF` terminated, in a
  fixed 8-byte field (`PLAYER_NAME_LENGTH + 1`). Never ASCII on the wire. Convert to ASCII only for
  logs.
- **Reserved** fields and bits: the sender writes `0`. The receiver **ignores** them (does not
  reject). This allows additive changes later without breaking a version, although any such
  change still bumps the version.
- Offsets in the tables below are **payload** offsets (byte 0 = first byte after the header).

---

## 3. Message header (4 bytes)

| Offset | Size | Type | Field | Rule |
|---|---|---|---|---|
| 0 | 1 | u8 | `type` | Message type, section 4 |
| 1 | 1 | u8 | `version` | `MP_PROTOCOL_VERSION` of the sender |
| 2 | 2 | u16 | `length` | Payload length in bytes (header excluded) |

Total packet size = `4 + length`. Constraints (checked before anything else):

- `4 <= packet->dataLength <= MP_MAX_PACKET_SIZE (512)`
- `length == packet->dataLength - 4` exactly
- `version == MP_PROTOCOL_VERSION`, **except** for `HELLO` (checked by the host through the frozen
  prefix, 6.1) and `REJECT` (always parsed by the client, 6.3).

---

## 4. Message types

| Type | Name | Direction | Channel | Payload size | State in which it is valid |
|---|---|---|---|---|---|
| `0x00` | *(invalid)* | — | — | — | never |
| `0x01` | `HELLO` | C → H | 0 | 32 | host: peer is pending (before handshake) |
| `0x02` | `WELCOME` | H → C | 0 | 12 + 20·N (N = 1..4) | client: CONNECTING, or reconnecting (7.6) |
| `0x03` | `REJECT` | H → C | 0 | 8 | client: CONNECTING, or reconnecting |
| `0x04` | `PLAYER_JOINED` | H → C | 0 | 20 | client: IN_SESSION |
| `0x05` | `PLAYER_LEFT` | H → C | 0 | 4 | client: IN_SESSION |
| `0x06` | `PLAYER_UPDATE` | C → H, H → C | 0 | 20 | IN_SESSION |
| `0x07` | `BYE` | both | 0 | 4 | any connected state |
| `0x08` | `PING` | both | 1 | 4 | IN_SESSION |
| `0x09` | `PONG` | both | 1 | 4 | IN_SESSION |
| `0x0A` | `RESUME` | C → H | 0 | 40 | host: peer is pending (instead of HELLO; PS-054, 6.17) |
| `0x0B` | `PLAYER_STATUS` | H → C | 0 | 4 | client: IN_SESSION (PS-054, 6.18) |
| `0x0C`–`0x0F` | reserved: session/control | | 0 | | |
| `0x10` | `PRESENCE` | C → H, H → C | 1 | 16 | IN_SESSION |
| `0x11`–`0x1F` | reserved: presence/world | | 1 | | |
| `0x20`–`0x3F` | reserved: social (chat, emotes) | | | | later |
| `0x40` | `INTERACT_REQUEST` | C → H, H → C | 0 | 8 | IN_SESSION (PS-041, 6.11) |
| `0x41` | `INTERACT_RESPONSE` | C → H, H → C | 0 | 8 | IN_SESSION (PS-041, 6.11) |
| `0x42` | `INTERACT_CANCEL` | C → H, H → C | 0 | 8 | IN_SESSION (PS-041, 6.11) |
| `0x43` | `TRAINER_CARD_DATA` | C → H, H → C | 0 | 56 | IN_SESSION (PS-042, 6.13) |
| `0x44` | `PARTY_DATA` | C → H, H → C | 0 | 8 + 100·M (M = 0..3) | IN_SESSION (PS-043, 6.14) |
| `0x45`–`0x5F` | **reserved: `INTERACT_*`** (Stage 4) | | 0 | | Stage 4 |
| `0x60` | `LINK_JOIN` | C → H | 0 | 4 | IN_SESSION (6.12) |
| `0x61` | `LINK_LEAVE` | C → H | 0 | 4 | IN_SESSION |
| `0x62` | `LINK_GROUP` | H → C | 0 | 4 | IN_SESSION |
| `0x63` | `LINK_END` | H → C | 0 | 4 | IN_SESSION |
| `0x64` | `LINK_OPEN` | C → H | 0 | 4 | IN_SESSION |
| `0x65` | `LINK_CLOSE` | C → H | 0 | 4 | IN_SESSION |
| `0x66` | `LINK_START` | C → H | 0 | 4 | IN_SESSION |
| `0x67` | `LINK_STATUS` | H → C | 0 | 4 | IN_SESSION |
| `0x68` | `LINK_BUS_START` | H → C | 0 | 4 | IN_SESSION |
| `0x69` | `LINK_BUS_END` | H → C | 0 | 4 | IN_SESSION |
| `0x6A` | `LINK_CMD` | C → H | 0 | 20 | IN_SESSION |
| `0x6B` | `LINK_ROW` | H → C | 0 | 8 + 16·N (N = 2..4) | IN_SESSION |
| `0x6C` | `TRADE_COMMIT` | both (READY C → H; COMMIT / ABORT H → C) | 0 | 8 | IN_SESSION (PS-044, 6.15) |
| `0x6D` | `TRADE_RECOVER` | both (routed by the host) | 0 | 32 | IN_SESSION (PS-050, 6.16) |
| `0x6E` | `LINK_DONE` | both (done C → H; done mask H → C) | 0 | 4 | IN_SESSION (PS-051, 6.12) |
| `0x6F` | `LINK_BLOCK` | both (a whole block C → H; relayed H → C) | 0 | 8 + n (n = 1..500) | IN_SESSION (PS-052, 6.12) |
| `0x70`–`0x7F` | reserved: `LINK_*` | | 0 | | |
| `0x80`–`0xEF` | reserved | | | | |
| `0xF0` | `DEBUG_ECHO` | both | 0 | 0..64 | only under `--mp-selftest` (PS-011). Session code drops it. |
| `0xF1`–`0xFE` | reserved: debug | | | | |
| `0xFF` | *(invalid)* | — | — | — | never |

C = client, H = host. A message type not listed as valid for the receiver's role, the receiver's
state, or the channel it arrived on is dropped and counted as malformed (8.2).

---

## 5. Shared structures

### 5.1 PlayerSummary (16 bytes)

Built from the local save by PS-013 (`MP_BuildPlayerSummary`, `MP_WritePlayerSummary`,
`MP_ReadPlayerSummary`). Never written into any save block.

| Offset | Size | Type | Field | Source (save) | Validation on receive |
|---|---|---|---|---|---|
| 0 | 8 | u8[8] | `name` | `gSaveBlock2Ptr->playerName` | see 5.1.1 |
| 8 | 1 | u8 | `gender` | `gSaveBlock2Ptr->playerGender` | `0` (MALE) or `1` (FEMALE) |
| 9 | 1 | u8 | `avatarStyle` | `MP_AVATAR_STYLE_PLAYER` (0) in M1 | `< MP_AVATAR_STYLE_COUNT` (= 1) |
| 10 | 1 | u8 | `badges` | **bitfield**: bit *i* = `FlagGet(FLAG_BADGE01_GET + i)`, i = 0..7. There is no count on the wire; the UI (PS-033) shows the popcount. | any value |
| 11 | 1 | u8 | `summaryFlags` | reserved, 0 | ignored |
| 12 | 4 | u32 | `trainerId` | `GetTrainerId(gSaveBlock2Ptr->playerTrainerId)`: `id[0] \| id[1]<<8 \| id[2]<<16 \| id[3]<<24`. Low 16 bits = public TID, high 16 = secret ID. | any value |

#### 5.1.1 Name rules

- Byte 0 must not be `EOS` (empty names are invalid; the game never produces one).
- There must be an `EOS` (0xFF) at some index 1..7. (7 visible characters max.)
- Every byte **before** the first `EOS` must be `< 0xF7`. Bytes `0xF7`–`0xFE` are text control
  codes (`DYNAMIC`, `KEYPAD_ICON`, `EXTRA_SYMBOL`, prompts, `EXT_CTRL_CODE` 0xFC,
  `PLACEHOLDER` 0xFD, newline 0xFE). A name containing them could make the text printer expand
  placeholders or read past buffers, so it is rejected.
- Bytes **after** the first `EOS` are ignored on receive and overwritten with `EOS` in memory.
  Senders pad with `EOS`.
- Receivers keep the name in the game charset. Debug logs convert it to ASCII (PS-013 helper).

### 5.2 PlayerEntry (20 bytes)

Used by `WELCOME`, `PLAYER_JOINED` and `PLAYER_UPDATE`.

| Offset | Size | Type | Field | Validation |
|---|---|---|---|---|
| 0 | 1 | u8 | `slot` | `< MP_MAX_PLAYERS` (further rules per message) |
| 1 | 1 | u8 | `epoch` | `!= 0` |
| 2 | 1 | u8 | `entryFlags` | bit 0 `MP_ENTRY_FLAG_HOST`: set **iff** `slot == 0`. Bit 1 `MP_ENTRY_FLAG_LOST` (PS-054): the player's connection is lost and the host holds its slot (7.6); never on slot 0 (BAD_FIELD); only meaningful in `WELCOME`, ignored elsewhere. Bits 2–7 reserved (ignored). |
| 3 | 1 | u8 | reserved | ignored |
| 4 | 16 | PlayerSummary | `summary` | 5.1 |

---

## 6. Messages

### 6.1 `HELLO` (0x01) — client → host, channel 0, payload 32 bytes

Sent by the client as soon as its ENet connection to the host is established.

| Offset | Size | Type | Field | Value |
|---|---|---|---|---|
| 0 | 4 | u8[4] | `magic` | bytes `50 53 4D 50` ("PSMP") |
| 4 | 2 | u16 | `protocolVersion` | `MP_PROTOCOL_VERSION` |
| 6 | 2 | u16 | reserved | 0 |
| 8 | 4 | u32 | `buildHash` | `MP_GetBuildHash()` (section 9) |
| 12 | 1 | u8 | `buildFlags` | bit 0 = built from a dirty tree (informational, **not** compared). Bits 1–7 reserved. |
| 13 | 3 | u8[3] | reserved | 0 |
| 16 | 16 | PlayerSummary | `summary` | the client's summary |

**Frozen prefix.** Header type `0x01` and HELLO payload bytes 0–11 (`magic`, `protocolVersion`,
reserved, `buildHash`) keep this exact layout in **every** future protocol version, so any host can
always tell an old or new client why it is rejected.

**Host processing, in this order:**

1. If the peer already sent a HELLO, or is already in the player table → BYE(PROTOCOL_ERROR) and
   disconnect.
2. `length < 12`, or `magic` wrong → `REJECT(BAD_DATA)`, disconnect.
3. `protocolVersion` differs from the host's version (`MP_PROTOCOL_VERSION`, or the host's
   `--mp-fake-protocol` value), or the header `version` byte differs from its low 8 bits →
   `REJECT(VERSION)`.
4. `buildHash != MP_GetBuildHash()` (the host's) → `REJECT(BUILD)`.
5. `length != 32` → `REJECT(BAD_DATA)`.
6. Summary fails 5.1 → `REJECT(BAD_DATA)`.
7. No free slot among 1..3 → `REJECT(FULL)`.
8. Otherwise accept: assign the **lowest free slot** in 1..3, assign a new epoch (7.4), add to the
   table, then send in this order:
   1. `WELCOME` to the new client,
   2. `PLAYER_JOINED` (the new entry) to every *other* client,
   3. the cached latest `PRESENCE` of every other player (host included) to the new client on
      channel 1. (Unreliable: if lost, the 1 s keep-alive repairs it.)
   4. queue `MP_EVENT_PLAYER_JOINED` locally on the host.

After any `REJECT`, the host calls `enet_peer_disconnect_later` and frees the pending peer.
If no HELLO arrives within `MP_CONNECT_TIMEOUT_MS` (5000 ms) of the ENet connect, the host resets
the peer silently.

### 6.2 `WELCOME` (0x02) — host → client, channel 0, payload 12 + 20·N bytes

| Offset | Size | Type | Field | Validation (client) |
|---|---|---|---|---|
| 0 | 1 | u8 | `yourSlot` | 1..3 |
| 1 | 1 | u8 | `yourEpoch` | `!= 0` |
| 2 | 1 | u8 | `playerCount` N | 1..`MP_MAX_PLAYERS`, and `length == 12 + 20*N` |
| 3 | 1 | u8 | `flags` | bit 0 `RESUMED` (PS-054): the answer to a `RESUME` (6.17). Bits 1–7 reserved (ignored). Was reserved before version 11. |
| 4 | 8 | u8[8] | `token` | PS-054: the recipient's reconnect token, random, **not all zero** (else BAD_FIELD). Every WELCOME carries a new one. |
| 12 | 20·N | PlayerEntry[N] | `players` | see below |

The table lists **every** occupied slot including the host (slot 0) and the recipient. Order is
ascending slot, but receivers must not rely on that.

Client validation (any failure → drop the connection with `BYE(PROTOCOL_ERROR)`, state ERROR,
`MP_EVENT_CONNECT_FAILED` with `MP_REASON_PROTOCOL_ERROR`):

- every entry passes 5.2; all `slot` values distinct;
- exactly one entry has slot 0 (and it carries the host flag);
- the entry with `slot == yourSlot` exists and its `epoch == yourEpoch`.

On success: state IN_SESSION, local slot = `yourSlot`, the table is replaced wholesale,
`MP_EVENT_SESSION_STARTED` is queued (no `PLAYER_JOINED` events for players who were already
there), and the client immediately sends its current `PRESENCE`. The client keeps `token` for a
`RESUME` (7.6); entries with `MP_ENTRY_FLAG_LOST` are players whose slot the host is holding.

A WELCOME with `RESUMED` set answers a `RESUME`; it is handled as described in 7.6 (same slot and
epoch required, the table is merged instead of replaced).

### 6.3 `REJECT` (0x03) — host → client, channel 0, payload 8 bytes

**Frozen layout** (never changes across versions; the client parses it whatever the header
`version` is, provided `length >= 8`).

| Offset | Size | Type | Field | Value |
|---|---|---|---|---|
| 0 | 1 | u8 | `reason` | `1` VERSION, `2` BUILD, `3` FULL, `4` BAD_DATA, `5` RESUME_REFUSED, `6` RESUME_MISMATCH (PS-054, answers to `RESUME` only, 6.17). `7`–`31` reserved. |
| 1 | 1 | u8 | reserved | 0 |
| 2 | 2 | u16 | `hostProtocolVersion` | host's protocol version (honours the host's `--mp-fake-protocol`) |
| 4 | 4 | u32 | `hostBuildHash` | host's `MP_GetBuildHash()` (logged by the client; the UI shows only the reason) |

Client: state ERROR, `MP_EVENT_CONNECT_FAILED` with `reason` = the wire reason (the values equal
`MP_REASON_VERSION..MP_REASON_BAD_DATA`). A reason outside 1..31 maps to `MP_REASON_BAD_DATA`.
The client then disconnects.

### 6.4 `PLAYER_JOINED` (0x04) — host → client, channel 0, payload 20 bytes

One PlayerEntry (5.2) for the player that just joined.

Validation: passes 5.2; `slot` in 1..3; `slot != ` own slot. If the slot is somehow already
occupied in the client's table, the entry is replaced (log a warning). Queues
`MP_EVENT_PLAYER_JOINED`. The slot's presence queue is cleared and `hasPresence = FALSE`.

### 6.5 `PLAYER_LEFT` (0x05) — host → client, channel 0, payload 4 bytes

| Offset | Size | Type | Field | Validation |
|---|---|---|---|---|
| 0 | 1 | u8 | `slot` | 1..3, `!=` own slot |
| 1 | 1 | u8 | `epoch` | must equal the table's epoch for that slot, else ignore (stale) |
| 2 | 1 | u8 | `reason` | `MP_REASON_LEFT` (40), `MP_REASON_CONNECTION_LOST` (33), `MP_REASON_KICKED` (35) or `MP_REASON_PROTOCOL_ERROR` (36). Any other value is treated as `CONNECTION_LOST`. |
| 3 | 1 | u8 | reserved | ignored |

Effect: the slot is freed, its presence queue cleared, `MP_EVENT_PLAYER_LEFT` queued (with a copy
of the name). The host is never announced with PLAYER_LEFT; host loss is `BYE` or a transport
disconnect.

### 6.6 `PLAYER_UPDATE` (0x06) — both directions, channel 0, payload 20 bytes

One PlayerEntry (5.2). Sent when the local summary changes mid-session (for example a new badge),
via `MP_RefreshLocalSummary()`. Rate limit: at most 1 per second per player (sender coalesces;
host drops extras).

- **Client → host:** the host **ignores** `slot`, `epoch` and `entryFlags` in the message and
  uses the sender's real slot. Summary must pass 5.1, else dropped and counted malformed. The
  host stores it and forwards it (with the correct slot/epoch/flags) to every *other* client, and
  queues `MP_EVENT_PLAYER_UPDATED` locally.
- **Host → client:** passes 5.2, slot occupied, epoch matches (else ignore). It may describe
  the host itself (slot 0) when the host's own summary changes. A client never receives its own
  update back. Queues `MP_EVENT_PLAYER_UPDATED`.

### 6.7 `BYE` (0x07) — both directions, channel 0, payload 4 bytes

| Offset | Size | Type | Field | Value |
|---|---|---|---|---|
| 0 | 1 | u8 | `reason` | `0` LEAVING, `1` HOST_SHUTDOWN, `2` PROTOCOL_ERROR. Others reserved (treated as LEAVING). |
| 1 | 3 | u8[3] | reserved | 0 |

The sender then calls `enet_peer_disconnect_later` (so the BYE is delivered first).

| Receiver | `reason` | Effect |
|---|---|---|
| host, from a client | LEAVING | remove client, broadcast `PLAYER_LEFT(MP_REASON_LEFT)` |
| host, from a client | other | as LEAVING |
| client, from host | HOST_SHUTDOWN / LEAVING | state ERROR, `MP_EVENT_DISCONNECTED(MP_REASON_HOST_CLOSED)` |
| client, from host | PROTOCOL_ERROR | state ERROR, `MP_EVENT_DISCONNECTED(MP_REASON_KICKED)` |

A transport-level disconnect without BYE is handled the same way with reason
`MP_REASON_CONNECTION_LOST` (and the host broadcasts `PLAYER_LEFT(CONNECTION_LOST)`).

### 6.8 `PRESENCE` (0x10) — both directions, channel 1 (unreliable sequenced), payload 16 bytes

Defined by PS-010, implemented by PS-030 (`src/mp/mp_presence.c`, capture in
`src/mp/mp_presence_capture.c`). PS-030 kept the byte layout unchanged and pinned down the field
semantics (6.8.1), so the protocol version stays 1. Packet = 4-byte header + 16 = 20 bytes.

| Offset | Size | Type | Field | Meaning | Validation |
|---|---|---|---|---|---|
| 0 | 1 | u8 | `slot` | player this describes | see below |
| 1 | 1 | u8 | `epoch` | that slot's epoch | see below |
| 2 | 2 | u16 | `seq` | per-sender sequence number, starts at 0 when the sender enters IN_SESSION, +1 per PRESENCE sent (keep-alives included), wraps | newer-than check, below |
| 4 | 1 | u8 | `mapGroup` | `gSaveBlock1Ptr->location.mapGroup`, or `0xFF` | see map rule |
| 5 | 1 | u8 | `mapNum` | `gSaveBlock1Ptr->location.mapNum`, or `0xFF` | see map rule |
| 6 | 2 | s16 | `x` | map-local tile x (`currentCoords.x - MAP_OFFSET`); during a step, the **destination** tile | `-16 <= x <= 1023` |
| 8 | 2 | s16 | `y` | map-local tile y, same rules | `-16 <= y <= 1023` |
| 10 | 1 | u8 | `facing` | `DIR_SOUTH` 1, `DIR_NORTH` 2, `DIR_WEST` 3, `DIR_EAST` 4 | 1..4 |
| 11 | 1 | u8 | `movement` | `MP_MOVE_*`: 0 NONE, 1 TURN, 2 WALK, 3 WALK_FAST, 4 WALK_FASTER, 5 JUMP, 6 JUMP_IN_PLACE, 7 WARP | `< MP_MOVE_COUNT` (8) |
| 12 | 1 | u8 | `avatarState` | `MP_AVATAR_*`: 0 ON_FOOT, 1 MACH_BIKE, 2 ACRO_BIKE, 3 SURFING, 4 UNDERWATER | `< MP_AVATAR_STATE_COUNT` (5) |
| 13 | 1 | u8 | `busy` | `MP_BUSY_*`: 0 NONE, 1 BATTLE, 2 MENU, 3 SCRIPT, 4 WARPING, 5 NOT_IN_FIELD | `< MP_BUSY_COUNT` (6) |
| 14 | 1 | u8 | `stepId` | incremented (mod 256) by the sender each time a new step / turn / jump / warp starts. Receivers use gaps to detect missed steps. | any |
| 15 | 1 | u8 | `presenceFlags` | reserved | ignored |

The wire enums are defined independently of the game's `PLAYER_AVATAR_STATE_*` /
`MOVEMENT_ACTION_*` numbers; PS-030 maps between them. Coordinates may be slightly outside the
map (by up to `MAP_OFFSET`) while the player crosses a map seam, hence the negative bound.
Consumers (PS-031) must additionally check `x`/`y` against the actual map layout size before
placing an object, and must never call `Get_MapHeader()` or similar on unvalidated values.

**Map rule:** either both `mapGroup` and `mapNum` are `0xFF` (`MP_MAP_NONE`; then `busy` must be
`NOT_IN_FIELD` and `x`, `y`, `facing`, `movement` are ignored), or `mapGroup < MAP_GROUPS_COUNT` and
`MP_IsValidMap(mapGroup, mapNum)` is true (PS-030 provides that check from a per-group map count
table). Exactly one of them being `0xFF` is invalid. When `mapGroup == 0xFF`, the `facing` check is
skipped.

**Client → host:** the host overwrites `slot` and `epoch` with the sender's real values (never
trusts them), validates the rest, applies the newer-than check against that sender's last
accepted `seq`, stores it as the cached presence for that slot, then forwards the packet
unchanged otherwise (same `seq`) to every other IN_SESSION client on channel 1.

A client that is still CONNECTING (its WELCOME was delayed or retransmitted while the host
already relays presence to it) drops PRESENCE silently as well, without counting it as malformed.

**Host → client:** drop silently (not counted as malformed; these are expected races) if `slot`
is not occupied, is the client's own slot, or `epoch` differs from the table. Then apply the
newer-than check per slot. Accepted updates replace `MpPlayer.presence`, set
`hasPresence = TRUE`, and are appended to the slot's FIFO (8 entries; on overflow drop the oldest
and set `presenceOverflow`).

**Newer-than check (RFC 1982 serial-number arithmetic, SERIAL_BITS = 16):**

```
bool8 MpSeqIsNewer(u16 s, u16 last)
{
    u16 d = (u16)(s - last);          // modulo 2^16
    return d != 0 && d < 0x8000;      // s is "greater than" last in serial arithmetic
}
```

- The first PRESENCE accepted for a slot after it is (re)assigned (new epoch) is always accepted,
  whatever its `seq`; it initialises `lastAcceptedSeq`.
- After that, accept only if `MpSeqIsNewer(seq, lastAcceptedSeq)`. Equal (duplicate) and older
  values are dropped silently. The case `d == 0x8000` is undefined in RFC 1982; here it is
  treated as **not newer** (dropped). That can only happen after 32768 lost packets, and the next
  keep-alive (1 s later) is accepted again.
- Wraparound: `seq` goes 65535 → 0 normally; `MpSeqIsNewer(0, 65535)` is TRUE.

### 6.8.1 Field semantics (PS-030)

Sender (`MP_SetLocalPresence` sanitises, so every PRESENCE sent passes the checks above):

| Field | How the capture hook fills it |
|---|---|
| `mapGroup`, `mapNum` | `gSaveBlock1Ptr->location` while the overworld callback (`CB2_Overworld`) runs. Outside it the **last field map** is kept. `0xFF`/`0xFF` + `NOT_IN_FIELD` before the overworld has been reached once (title, intro, main menu). An invalid map is sent as `0xFF`/`0xFF`. |
| `x`, `y` | player object `currentCoords - MAP_OFFSET` (already the destination during a step), clamped to `-16..1023`. Kept from the last field tick outside the overworld. |
| `facing` | player object `facingDirection` (1..4; anything else is sent as `DIR_SOUTH`). |
| `movement` | The kind of the **last step that started**. A 1-tile move is `WALK` (`WALK_NORMAL`, `WALK_SLOW`, 1-tile jumps), `WALK_FAST` (`WALK_FAST`, `PLAYER_RUN`, `RIDE_WATER_CURRENT`, `SLIDE`: running, surfing, acro bike, currents, ice) or `WALK_FASTER` (`WALK_FASTER`: mach bike top speed). A straight 2-tile move is `JUMP` (ledge). Starting a `JUMP_IN_PLACE_*` / `ACRO_WHEELIE_HOP_FACE_*` action is `JUMP_IN_PLACE`. A facing change in place is `TURN`. Any other position change, a map change that is not a seam step, or the first field tick after a load at another tile is `WARP`. Walking across a **map seam** (the map changes while the overworld callback keeps running and the player is mid-step) is reported as the step it is, with coordinates already in the new map. It returns to `NONE` after 2 consecutive ticks without a held movement in progress (so consecutive steps do not flicker), and is `NONE` outside the overworld. |
| `avatarState` | `gPlayerAvatar.flags`: UNDERWATER > SURFING > MACH_BIKE > ACRO_BIKE > ON_FOOT. |
| `busy` | Overworld: `MENU` while the start menu task runs, `SCRIPT` while field controls are locked (scripts, cutscenes, door and warp animations, battle transitions), else `NONE`. Elsewhere: `BATTLE` if `gMain.inBattle`, `WARPING` if the save's map already differs from the last field map (`WarpIntoMap()` ran and the new map is loading), else `MENU` (bag, party, PC, summary, ...). |
| `stepId` | +1 (mod 256) each time a new step / turn / jump / jump-in-place / warp starts. |

**Sending** (`MP_Update`, at most once per call): the first PRESENCE goes out as soon as the
session starts (host: the first `MP_Update` after `MP_Host`; client: right after WELCOME). After
that, it is sent when any field except `seq` differs from the last one sent and at least 50 ms have
passed, or after 1000 ms unchanged (keep-alive). The host relays an accepted client PRESENCE with the
same `seq`, re-serialised from the validated message (so reserved bits are 0 and ignored fields
normalised).

**Receiver storage:** `MpPlayer.presence` (latest) and a per-slot FIFO of 8
(`MP_PopRemotePresence`; overflow drops the oldest and sets `presenceOverflow`). The host keeps the
same for every client, because the host is a player too. The FIFO and the seq state are cleared
whenever the slot is (re)assigned or freed. Clients receive every player's presence whatever map
they are on. Filtering by map is the consumer's job (PS-031).

**`--mp-debug`:** every PRESENCE sent or accepted that differs from the previous one is logged as
`presence sent|recv slot S seq N map G.M (x,y) facing F MOVE state A busy B step K`, and stale drops
are logged. Every 10 s each peer gets a line
`presence peer P (slot S): out X B/s Y pkt/s (~W B/s on the wire), in ...`. The byte count is
message bytes including the 4-byte header; "on the wire" adds an estimated 40 bytes of ENet +
UDP/IPv4 per packet. A totals line follows (sent, accepted, stale, ignored, invalid).

Why keep `seq` although channel 1 is ENet unreliable-*sequenced* (ENet already drops
out-of-order packets per peer and channel): (a) on a client, presence of different players is
relayed by the host and ENet's ordering is per sender, not per original player; (b) together with
`epoch` it rejects stale packets across a slot being freed and reused or a reconnect; (c) it shows
loss and reordering in `--mp-debug` logs and counters.

### 6.9 `PING` (0x08) / `PONG` (0x09) — both directions, channel 1, payload 4 bytes

| Offset | Size | Type | Field |
|---|---|---|---|
| 0 | 4 | u32 | `timeMs`: for PING, the sender's `MpTransport_GetTimeMs()`; for PONG, the value copied from the PING being answered |

- The host pings every client, and every client pings the host, every `MP_PING_INTERVAL_MS`
  (1000 ms). Clients do not ping each other (they are not connected).
- On PING, reply with PONG immediately (but ignore PINGs arriving less than 250 ms after the
  previous one from the same peer).
- On PONG, `rtt = now - timeMs` (u32 wrap-safe). If `rtt > 60000`, discard it. Otherwise store it
  in `MpPlayer.pingMs`: on the host, for each client's entry; on a client, on the host's entry
  (slot 0). Other clients' `pingMs` stay 0 on a client in M1.

### 6.10 `DEBUG_ECHO` (0xF0) — selftest only

Payload 0..64 arbitrary bytes, channel 0. Only handled by the PS-011 `--mp-selftest` loopback test.
In a normal session it is dropped and counted malformed.

### 6.11 `INTERACT_REQUEST` (0x40) / `INTERACT_RESPONSE` (0x41) / `INTERACT_CANCEL` (0x42) — both directions, channel 0, payload 8 bytes

PS-041. A player asks another player to **trade** or **battle**; the host routes everything
(clients never talk to each other). Implemented in `src/mp/mp_interact.c`; UI in
`src/mp/mp_menu_interact.c`. These messages only negotiate; PS-044 / PS-045 start the actual
trade / battle after an `ACCEPTED`.

All three share one layout:

| Offset | Size | Type | Field | Rule |
|---|---|---|---|---|
| 0 | 1 | u8 | `slot` | The **other party** from the receiver's point of view (table below). `< 4` |
| 1 | 1 | u8 | `epoch` | Epoch of `slot`. `!= 0` |
| 2 | 1 | u8 | `kind` | `1` TRADE, `2` BATTLE (a single link battle, PS-045), `3` TRAINER_CARD (PS-042, 6.13), `4` VIEW_PARTY (PS-043, 6.14), `5` BATTLE_DOUBLE (a double link battle, PS-045, version 8; prompted and answered like BATTLE). Others rejected |
| 3 | 1 | u8 | `requestId` | Chosen by the requester (any value), echoed in RESPONSE / CANCEL |
| 4 | 1 | u8 | `result` | RESPONSE only: `1` ACCEPTED, `2` DECLINED, `3` BUSY, `4` TIMEOUT, `5` UNAVAILABLE; others rejected. REQUEST / CANCEL: reserved, sender writes 0, receiver ignores |
| 5 | 3 | u8[3] | reserved | 0 |

| Message | Sender → receiver | `slot` means |
|---|---|---|
| REQUEST | requester → host | the target |
| REQUEST | host → target | the requester |
| RESPONSE | target → host | the requester |
| RESPONSE | host → requester | the target |
| CANCEL | requester → host | the target |
| CANCEL | host → target | the requester |

**Host rules.** The sender of a C → H message is always the peer's real slot; `slot`/`epoch` in
the message only name the other party, and the host checks them against its table.

- One request in flight **per requester** (so also per pair). The host keeps a route per
  requester slot: `{target, epochs, kind, requestId, start time}`.
- REQUEST is refused, with a RESPONSE sent straight back to the requester, when:
  - the requester already has a request in flight, or sent its previous request less than
    `MP_INTERACT_MIN_GAP_MS` (1000 ms) ago → `UNAVAILABLE` (rate limit, not counted malformed);
  - the target is the requester, an empty slot, or its epoch does not match → `UNAVAILABLE`;
  - requester and target are not on the same map by their latest PRESENCE (`mapGroup`,
    `mapNum`, neither `MP_MAP_NONE`) → `UNAVAILABLE`;
  - someone else is already asking the target, or the target has its own request in flight →
    `BUSY`.
- Otherwise the host stores the route and forwards REQUEST to the target (or handles it itself
  when the host is the target).
- RESPONSE from the target is forwarded only if it matches the route (requester, target,
  `requestId`); anything else is stale and dropped silently. CANCEL likewise.
- No answer within `MP_INTERACT_TIMEOUT_MS + MP_INTERACT_HOST_GRACE_MS` (15 + 2 s): the host
  sends CANCEL to the target and RESPONSE `TIMEOUT` to the requester.
- A player leaving the table (BYE, timeout, kick): its own request is CANCELled at its target,
  and every requester asking it gets RESPONSE `UNAVAILABLE`.

**Endpoint rules.**

- Target: answers with RESPONSE `ACCEPTED` / `DECLINED` / `BUSY`. Its session answers
  `TIMEOUT` itself after `MP_INTERACT_TIMEOUT_MS` (15 s). The UI answers `BUSY` when the player
  stays in a battle, menu or script for ~3 s after the request arrives. A REQUEST naming a
  requester that is no longer in the table is dropped. A second REQUEST while one is pending
  is answered `BUSY` (the host never sends one).
- Requester: gives up locally (`TIMEOUT`) after `MP_INTERACT_TIMEOUT_MS +
  MP_INTERACT_CLIENT_GRACE_MS` (15 + 5 s) without an answer. A RESPONSE not matching its
  pending request is dropped. `PLAYER_LEFT` for the target ends the request (`UNAVAILABLE`).
- A client receiving any INTERACT message whose `slot` is its own slot counts it malformed.

### 6.12 Virtual link cable `LINK_*` (0x60–0x6B, 0x6E, 0x6F) — PS-046, PS-051, PS-052, channel 0

Emerald's cable link (`src/link.c`) runs unchanged over the session; see
`research/PS-040-link-report.md` section 2 for the cable semantics being reproduced. The
**host is the bus**: it sequences every link group, including groups it is not a member of
(Director ruling). Game side: `MP_LinkMain1` (`src/mp/mp_link_cable.c`) replaces the SIO
`LinkMain1`; session side: `src/mp/mp_link.c`.

**Concepts.**
- A **link group** is 2..4 slots. It forms when every slot in a mask has sent `LINK_JOIN` with that
  same mask and none of them is in a group. **Link id** = rank of the slot in the mask (lowest slot
  = link id 0 = the link master, `GetMultiplayerId()`). A slot is in at most one group.
- Each member counts its own **open sequence** (u8, +1 per `OpenLink`). The host records which
  members are open with which sequence.
- A **bus** is one run of the cable between an open and a close. The host gives each bus an
  **epoch** (u8, never 0). `LINK_CMD`, `LINK_ROW` and `BUS_END` carry it; anything for an older
  epoch is a stale race and is dropped **silently** (not malformed).
- A **row** is one 8-word command per link id (zeros for "nothing"), sent to **every** member,
  the sender included, like the SIO multi-mode registers. Members consume one row per game tick.

**Control messages**: payload 4 bytes, one layout for all of them:

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 1 | `groupId` | host-assigned, never 0. `JOIN` / `LEAVE`: reserved (0). `END`: 0 = "your request was refused". |
| 1 | 1 | `arg1` | per type, below |
| 2 | 1 | `arg2` | per type, below; reserved (0) where unused |
| 3 | 1 | reserved | 0, ignored |

| Type | Dir | `arg1` | `arg2` | Validation (parser, pure) |
|---|---|---|---|---|
| `0x60` `LINK_JOIN` | C → H | member slot mask | — | mask only in bits 0..3, 2..4 bits set |
| `0x61` `LINK_LEAVE` | C → H | — | — | — |
| `0x62` `LINK_GROUP` | H → C | member slot mask | recipient's link id | `groupId != 0`, mask as JOIN, link id < member count |
| `0x63` `LINK_END` | H → C | reason: 1 LEFT, 2 LOST, 3 OVERFLOW, 4 REFUSED, 5 DONE (PS-051, a normal end; 6..31 reserved) | — | reason != 0 |
| `0x64` `LINK_OPEN` | C → H | open sequence | — | `groupId != 0` |
| `0x65` `LINK_CLOSE` | C → H | open sequence | — | `groupId != 0` |
| `0x66` `LINK_START` | C → H | open sequence | — | `groupId != 0` |
| `0x67` `LINK_STATUS` | H → C | open mask by link id | — | `groupId != 0`, mask < 0x10 |
| `0x68` `LINK_BUS_START` | H → C | bus epoch | recipient's open sequence | `groupId != 0`, epoch != 0 |
| `0x69` `LINK_BUS_END` | H → C | bus epoch | — | `groupId != 0`, epoch != 0 |

**`0x6A` `LINK_CMD`** (C → H), payload 20 bytes: `groupId` u8, `busEpoch` u8, reserved u16,
`cmd` u16[8] (one link command, `gSendCmd`). Valid: `groupId != 0`, `busEpoch != 0`, and the
command is not all zeros (an all-zero command is never sent; the cable drops empty rows too).

**`0x6B` `LINK_ROW`** (H → C), payload 8 + 16·N bytes: `groupId` u8, `busEpoch` u8, `count` N u8
(2..4), reserved u8, `rowSeq` u16 (per bus, from 0, for logs), reserved u16, then N × u16[8] in link
id order. Valid: `groupId != 0`, `busEpoch != 0`, 2 <= N <= 4 and `length == 8 + 16·N` exactly.

**Host processing** (after the parser; the sender's slot is the real one, never a field):

| Received | Action |
|---|---|
| `JOIN(mask)` | Mask without the sender, or with an empty slot → `END(0, REFUSED)` to the sender. If the sender is in a group, that group ends (`END(LEFT)` to the others). Store the request; form the group if every slot in the mask has asked for the same mask. Forming: new `groupId`, `GROUP(id, mask, linkId)` to each member. |
| `LEAVE` | Drop the request; if in a group, the group ends: `END(LEFT)` to the other members. |
| `OPEN(g, seq)` | `g` not the sender's group → ignore. If the bus is up it ends (`BUS_END`). Mark open with `seq`, send `STATUS(openMask)` to all members. If a start is pending and all are open, start the bus. |
| `CLOSE(g, seq)` | Ignore unless open with that `seq`. Mark closed, cancel a pending start, end the bus if up (`BUS_END(epoch)` to all members), send `STATUS`. |
| `START(g, seq)` | Only from link id 0, open with that `seq`, bus not up; else ignore. All open → **start the bus**: new epoch, empty queues, `BUS_START(epoch, member's seq)` to each member. Otherwise remember the start until all are open. |
| `CMD(g, epoch, cmd)` | Ignore unless the bus `g`/`epoch` is up. Queue it for the sender's link id (128 per member). Queue full → the group ends with `END(OVERFLOW)` to all members. **PS-047 pre-filter** (host behaviour, no wire change): an `INIT_BLOCK` (`0xBBBB`) whose size word is above `0x4000`, or a `SEND_BLOCK_REQ` (`0xCCCC`) whose type is 5 or more, is never queued; the group ends with `END(LEFT)` to all members, sender included. |
| a player leaves the table | Requests that include the slot get `END(0, LOST)`; its group ends with `END(LOST)` to the others. |

Rows: once per `MP_Update`, for every group whose bus is up, while any member's queue is not
empty: take at most one command from each member's queue (zeros for an empty queue) and send
`ROW` to every member. Each member sends at most one command per game tick and consumes one row
per tick, so pairing the queues keeps the row rate at the busiest member's command rate, as on
the cable. The host's own member (slot 0) is served by direct calls, never the network.

**Member processing.** `GROUP` is accepted only while requested with that exact mask and link id;
`END` for our group or for our pending request (group 0) ends it; `STATUS` updates the open count
used for the handshake player count; `BUS_START` is accepted only while open with that sequence
(rows of the new bus follow it on the same ordered channel); `ROW` and `BUS_END` only for the
current bus. Rows are queued (`MP_LINK_RX_QUEUE_SIZE` = 256; overflow ends the link with
`RX_OVERFLOW` and sends `LEAVE`). The game is told at most 50 (`QUEUE_CAPACITY`) of them.
Watchdog: own commands not echoed in a row for 10 s (`MP_LINK_WATCHDOG_MS`) → `LEAVE`, link
lost. A group or bus that ends while the game's cable is open sets the game's hardware-error bit.

**Typical exchange** (A = link id 0, B = link id 1; both have called `MP_LinkJoin(mask)`):

```
A,B -> H  JOIN(mask)                      H -> A,B  GROUP(g, mask, linkId)
A,B -> H  OPEN(g, seqA/seqB)              H -> A,B  STATUS(g, 0b11)        (after each OPEN)
A   -> H  START(g, seqA)                  H -> A,B  BUS_START(g, e, seq)
A,B -> H  CMD(g, e, cmd) ...              H -> A,B  ROW(g, e, n, {cmdA, cmdB}) ...
A,B -> H  CLOSE(g, seq)                   H -> A,B  BUS_END(g, e), STATUS(g, ...)
```

**Agreed end of a link activity (PS-051, version 9).** Vanilla ends a cable activity with the
close-link handshake, which assumes GBA lockstep. Over this host-sequenced cable one member closes
first and the other can still be mid-handshake when the bus or the group goes away (BUG-004/005/007).
So once the players have agreed to stop (a direct trade: the leader sends `LINKCMD_BOTH_CANCEL_TRADE`
and the follower receives it; a direct battle: `ENDLINKBATTLE`), each member ends on its own:
it waits until its last command is out, sends `LINK_DONE(g)` and then closes its cable (`LINK_CLOSE`).
It does **not** send `LINK_LEAVE`.

| Type | Dir | `arg1` | `arg2` | Validation (parser, pure) |
|---|---|---|---|---|
| `0x6E` `LINK_DONE` | C → H | — (0) | — | `groupId != 0`, `arg1` < 0x10 |
| `0x6E` `LINK_DONE` | H → C | done mask by link id | — | `groupId != 0`, `arg1` < 0x10 |

Host: `DONE(g)` from a member of `g` (ignored otherwise, and when repeated) first emits every row
still queued for the bus. Then it marks that link id as done and sends `DONE(g, doneMask)` to every
member. When all members are done, the group ends with `END(DONE)` (reason 5). Otherwise it ends
`MP_LINK_DONE_GRACE_MS` (3000 ms) after the first `DONE`. Since version 9 the host also emits the
queued rows before it acts on a `CLOSE`, so a member that closes right after its last block never
loses that block.

Member: a `DONE` naming another link id, our own agreement, or `END(DONE)` makes the member
*finishing*. While finishing, `BUS_END` and the group ending are the normal end, not a hardware
error. The rows already queued still play, and then the cable stays silent until the game closes
it. After the field return the member waits for `END(DONE)`, and leaves the group itself only if
that has not come within ~5 s. Order on channel 0 (the leader closes first):
`ROW…(BOTH_CANCEL)`, `DONE(g, 0b01)`, `BUS_END(g, e)`, and later `END(g, DONE)`. So a slow follower
knows the leader is done before it reaches the `BUS_END`.

**Whole blocks (PS-052, version 10).** On the GBA cable a `SendBlock` is an `INIT_BLOCK` row, three
idle ticks and one `CONT_BLOCK` row per 14 bytes (a 256-byte block is about 23 ticks), and every
row goes through the host. Since version 10 a member sends a block of 1..500 bytes
(`MP_LINK_BLOCK_MAX`) as **one** `LINK_BLOCK` instead; bigger blocks still use the rows. Both work on
every version-10 member, so members may differ (`--mp-link-rows` forces the rows, for comparison).

**`0x6F` `LINK_BLOCK`**, payload 8 + n bytes:

| Offset | Size | Field | Notes |
|---|---|---|---|
| 0 | 1 | `groupId` | != 0 |
| 1 | 1 | `busEpoch` | != 0 |
| 2 | 1 | `linkId` | C → H: 0, ignored (the host uses the sender's real link id). H → C: the sender's link id, < 4 |
| 3 | 1 | reserved | 0, ignored |
| 4 | 2 | `size` | n, 1..500 |
| 6 | 2 | reserved | 0, ignored |
| 8 | n | `data` | the block |

Valid: the fields above and `length == 8 + size` exactly (TRUNCATED / OVERSIZED otherwise).
The largest message is 4 + 8 + 500 = 512 bytes, the packet limit.

Host: a `BLOCK(g, epoch)` from a member of the bus `g`/`epoch` (stale otherwise, dropped silently)
first **emits every row still queued** for the bus, then sends the block, with the sender's link
id, to every member, the sender included (like its rows). Each member's commands and blocks reach
the host in order on channel 0, so a group's bus stays **one sequenced stream**: every member sees
the same rows and blocks in the same order.

Member: a block for the current bus is queued in the same receive queue as the rows (it counts
towards `MP_LINK_RX_QUEUE_SIZE`; at most 64 blocks wait, overflow ends the link with `RX_OVERFLOW`).
Our own block coming back counts for the watchdog like our own command in a row. The game side
(`MP_LinkMain1`) hands the blocks queued ahead of the next row to `link.c`, which fills
`sBlockRecv`, `gBlockRecvBuffer` (or `gDecompressionBuffer` above 256 bytes) and the received flag
(or completes the player-data exchange) exactly as the last `CONT_BLOCK` row would. Pacing keeps the
GBA guarantees: at most one block per link id per tick, and while the game has not taken the
previous block from that link id (its received flag is still set), the next one waits at most the
ticks the GBA cable needed for it. On the sending side the link task is finished at once.

Row pacing (no wire change): rows that pile up in the receive queue are played up to 8 per tick, but
only rows that carry nothing but block data (`INIT_BLOCK`, `CONT_BLOCK`, zeros), never right after
another command, and only until a block completes in that tick. Every other command still reaches
the game on its own tick, followed by its link callback, as on the GBA.

Malformed (parser failure, wrong direction, wrong channel, an unassigned `0x70`–`0x7F` type; `0x6C` is 6.15, `0x6D` 6.16):
dropped and counted against the 8.2 budget. Stale (other group, old epoch, old sequence): silent.

### 6.13 `TRAINER_CARD_DATA` (0x43) — both directions, channel 0, payload 56 bytes

PS-042. Viewing another player's trainer card reuses the 6.11 request flow with
`kind = 3` TRAINER_CARD, so it gets the same host checks (same map, one request in flight per
requester, 1 s gap), timeouts and leave handling. What is different:

- **No prompt.** A trainer card is public: the target's *session* answers by itself the moment
  the REQUEST arrives, whatever the player is doing (battle, menu, script), and queues no
  `MP_EVENT_INTERACT_REQUEST`. It sends `TRAINER_CARD_DATA`, then `INTERACT_RESPONSE`
  `ACCEPTED`. Both are on reliable channel 0, which ENet keeps in order, so the data always
  arrives before the RESPONSE, also after the host relays them.
- **Never busy.** A TRAINER_CARD request is not refused with `BUSY` (it does not touch the
  target's trade / battle prompt), and a TRAINER_CARD route does not make its target or its
  requester busy for trade / battle requests.
- **The data is routed exactly like the RESPONSE it precedes** (6.11 table): `slot` is the
  other party from the receiver's point of view (C → H: the requester; H → C: the target),
  `epoch` that slot's epoch, `requestId` the request's id.

| Offset | Size | Type | Field | Rule on receive |
|---|---|---|---|---|
| 0 | 1 | u8 | `slot` | `< 4` |
| 1 | 1 | u8 | `epoch` | `!= 0` |
| 2 | 1 | u8 | `requestId` | any |
| 3 | 1 | u8 | reserved | 0, ignored |
| 4 | 1 | u8 | `gender` | `0` MALE or `1` FEMALE, else rejected (indexes the trainer pic tables) |
| 5 | 1 | u8 | `stars` | `0..4`, else rejected (indexes the 5 card palettes, and is a tilemap width) |
| 6 | 1 | u8 | `flags` | bit 0 hasPokedex, bit 1 caughtAllHoenn, bit 2 hasAllPaintings, bit 3 hasAllFrontierSymbols; bits 4–7 reserved, ignored |
| 7 | 1 | u8 | reserved | 0, ignored |
| 8 | 2 | u16 | `hofDebutHours` | clamped to 999 |
| 10 | 1 | u8 | `hofDebutMinutes` | clamped to 59 |
| 11 | 1 | u8 | `hofDebutSeconds` | clamped to 59 |
| 12 | 2 | u16 | `caughtMonsCount` | clamped to 999 |
| 14 | 2 | u16 | `trainerId` | public 16-bit ID, any |
| 16 | 2 | u16 | `playTimeHours` | clamped to 999 |
| 18 | 1 | u8 | `playTimeMinutes` | clamped to 59 |
| 19 | 1 | u8 | reserved | 0, ignored |
| 20 | 2 | u16 | `linkBattleWins` | clamped to 9999 |
| 22 | 2 | u16 | `linkBattleLosses` | clamped to 9999 |
| 24 | 2 | u16 | `battleTowerWins` | clamped to 9999 |
| 26 | 2 | u16 | `battleTowerStraightWins` | clamped to 9999 |
| 28 | 2 | u16 | `contestsWithFriends` | clamped to 999 |
| 30 | 2 | u16 | `pokeblocksWithFriends` | any |
| 32 | 2 | u16 | `pokemonTrades` | any |
| 34 | 2 | u16 | `frontierBP` | clamped to 9999 |
| 36 | 4 | u32 | `money` | clamped to 999999 |
| 40 | 8 | u16[4] | `easyChatProfile` | each word: `0xFFFF` (empty) or `group = word >> 9` `< EC_NUM_GROUPS` (22); any other word becomes `0xFFFF`. An unknown index inside a valid group is safe: the game prints "???" (`CopyEasyChatWord`). |
| 48 | 8 | u8[8] | `name` | the 5.1.1 rules (else rejected); bytes after the first EOS stored as EOS |

**Sender.** `MP_BuildLocalTrainerCard` (`src/mp/mp_playerdata.c`) reads the save through the
game's own code (`TrainerCard_GenerateCardForLinkPlayer` + `CopyTrainerCardData(…,
VERSION_EMERALD)`, the same data the cable and Union Room exchange) and copies it field by field
(never a raw struct; `struct TrainerCard` has padding). It takes the name, gender and trainer ID
from the player summary (so the `--name` override and the summary's name sanitising apply) and
applies the clamps above itself, so what it sends always parses unchanged. The card has no badge
field: in link mode the card screen shows no badges (vanilla behaviour, PS-040 Q6).

**Host.** Parses (a parse failure counts malformed against the sender), then forwards only along
the live route of the requester named by `slot`: the sender must be that route's target, the
`requestId` must match and the route's kind must be TRAINER_CARD; at most **one** data message
per route. No matching route (cancelled, timed out, requester gone) → dropped silently. A route
of another kind, or a second data message → malformed. The forwarded copy is re-serialised from
the parse (clamped values, reserved bytes 0) with `slot` / `epoch` rewritten to the target's. When
the host is the requester it keeps the data itself; when it is the target it sends its card to
the requester directly.

**Requester.** Accepts the data only while its own TRAINER_CARD request to that `slot` with that
`requestId` is pending (else stale, silent; a `slot` equal to its own slot is malformed). A
second copy, or data for a request of another kind, marks the transfer bad. When the RESPONSE
arrives: `ACCEPTED` with exactly one valid card → `MP_EVENT_INTERACT_RESULT` `ACCEPTED` and the
card is available from `MP_GetViewedTrainerCard()`; `ACCEPTED` with the card missing, invalid
or duplicated → the local result `MP_INTERACT_RESULT_BAD_DATA` (7, never on the wire) and
nothing is shown. Any other result discards the data. The card lives in session memory only and
is never written to the save (AD-10).

**Display** (`src/mp/mp_menu_interact.c`, `mp_playerdata.c`). The UI copies the card out of
the session, fades out and shows it with `trainer_card.c`'s link-partner mode
(`ShowTrainerCardInLink(1, CB2_ReturnToField)`; front and back, A flips, B closes). It borrows
`gTrainerCards[1]` and `gLinkPlayers[1].language` only for that call, which copies them at once,
and restores them right after. The conversion clamps `stars` and `gender` once more.

### 6.14 `PARTY_DATA` (0x44) — both directions, channel 0, payload 8 + 100·M bytes

PS-043. Viewing another player's party reuses the 6.11 request flow with `kind = 4`
VIEW_PARTY. Unlike a trainer card it **needs consent**: the target is prompted exactly like for a
trade ("NAME wants to see your POKéMON. Allow?", YES / NO, auto-BUSY, 15 s timeout) and the
usual busy rules apply. When the target answers `ACCEPTED`, its session first sends the party as
`PARTY_DATA` parts, then the `INTERACT_RESPONSE`, all on reliable ordered channel 0. Routing of
`slot` / `epoch` / `requestId` is the same as for `TRAINER_CARD_DATA` (6.13).

**Why parts.** A mon travels as the game's own **raw 100-byte `struct Pokemon`** (the encrypted,
checksummed link-trade format; its layout is only u8/u16/u32 fields at natural alignment, so it
is the same on 32- and 64-bit builds, static-asserted in `mp_playerdata.c`). Six mons are 600
bytes, which does not fit in one packet (`MP_MAX_PACKET_SIZE` 512, also ENet's
`maximumPacketSize`). The party is split into `ceil(count / 3)` parts (one part for an empty
party): at most 2 parts of at most 312 bytes each.

| Offset | Size | Type | Field | Rule on receive |
|---|---|---|---|---|
| 0 | 1 | u8 | `slot` | `< 4` |
| 1 | 1 | u8 | `epoch` | `!= 0` |
| 2 | 1 | u8 | `requestId` | any |
| 3 | 1 | u8 | reserved | 0, ignored |
| 4 | 1 | u8 | `partyCount` | `0..6`, the same in every part |
| 5 | 1 | u8 | `partIndex` | `< max(1, ceil(partyCount / 3))`; parts are sent and must arrive in order 0, 1 |
| 6 | 1 | u8 | `monCount` M | exactly `min(3, partyCount − 3·partIndex)` |
| 7 | 1 | u8 | reserved | 0, ignored |
| 8 | 100·M | struct Pokemon[M] | `mons` | party mons `3·partIndex .. 3·partIndex + M − 1`, raw; each one checked by the requester (6.14.1) |

`length` must be exactly `8 + 100·M` (shorter: truncated; longer: oversized). The pure parser
(`MpProto_ParsePartyData`) checks the layout only; the mons are game data and are checked by
`MP_ValidateMon` (game code, `src/mp/mp_playerdata.c`).

#### 6.14.1 Mon validation (`MP_ValidateMon`, requester side, every mon, on a local copy)

Reject the mon if any of these fail:

- the raw `isBadEgg` bit is clear, and after `GetMonData` has decrypted the copy the **checksum
  matches** (a mismatch sets `isBadEgg`; any single flipped bit in the checksum or in the 48
  encrypted bytes is always caught);
- `hasSpecies` is set; species `1 .. NUM_SPECIES − 1` and not one of the unused
  `SPECIES_OLD_UNOWN_B..Z` slots (table indices for stats, pics, cries, names);
- level `1 .. MAX_LEVEL` (100) (`gExperienceTables[..][level + 1]`);
- `hp <= maxHP`;
- held item `< ITEMS_COUNT`; each of the 4 moves `< MOVES_COUNT` (`gMoveNames`, `gBattleMoves`,
  `gContestMoves` indices);
- markings `< 16` (sprite animation index); language `<= LANGUAGE_SPANISH` (7);
- nickname (10 bytes) and OT name (7 bytes): no byte `>= 0xF7` (text control codes) before the
  first EOS. A name without EOS is fine: `GetMonData` copies and terminates it.

Fields the summary screen already guards (met location `>= MAPSEC_NONE`, met game, ribbons,
status) are not restricted. The sender only includes mons of its own party that pass the same
checks (`MP_BuildLocalParty`; a bad egg is left out rather than making the party unviewable).

**Host.** Parses the layout (failure: malformed against the sender) and forwards each part only
along the live VIEW_PARTY route of the requester named by `slot`: sender = the route's target,
matching `requestId`, `partIndex` equal to the number of parts already forwarded, at most 2.
No route → dropped silently. A route of another kind, a part out of order or a third part →
malformed. The forwarded copy is re-serialised from the parse with `slot` / `epoch` rewritten.
The host does not judge the mons; the requester does.

**Requester.** Accepts parts only while its own VIEW_PARTY request to that `slot` with that
`requestId` is pending (else stale, silent). Each part must have the next `partIndex` and the
same `partyCount`, and every mon in it must pass 6.14.1; otherwise the **whole transfer is
rejected**. When the RESPONSE arrives: `ACCEPTED` with all parts and `partyCount` mons received →
`MP_EVENT_INTERACT_RESULT` `ACCEPTED`, and the mons are available from `MP_GetViewedParty()`;
anything missing or rejected → `MP_INTERACT_RESULT_BAD_DATA`, nothing is shown. The mons live in
session memory only, never in `gPlayerParty` / `gEnemyParty`, never in the save (AD-10).

**Display.** The UI copies the mons out of the session, fades out and opens the summary screen
on a private static `struct Pokemon[6]` (`ShowPokemonSummaryScreen(SUMMARY_MODE_LOCK_MOVES, …)`,
the battle mode, so moves cannot be reordered); Up / Down switches mons, B returns to the field.
An empty party only shows "NAME has no POKéMON.".

### 6.15 `TRADE_COMMIT` (0x6C) — both directions, channel 0, payload 8 bytes (PS-044; `tradeId` since PS-050)

The host-arbitrated commit of a direct trade's save (Director ruling on PS-044). A direct trade
(TRADE request accepted, PS-041) runs Emerald's own trade over the virtual cable (6.12) in a
2-member link group. Its save is two-phase (`CB2_SaveAndEndTrade`): every sector is written,
the last one without its signature, then the signature is written. The signature is the commit:
until it is written the previous save is the valid one. For a direct trade the vanilla standby
between "unsigned" and "signed" is replaced by this exchange.

| Offset | Size | Type | Field | Rule |
|---|---|---|---|---|
| 0 | 1 | u8 | `op` | 1 READY (C → H), 2 COMMIT (H → C), 3 ABORT (H → C); anything else rejected |
| 1 | 1 | u8 | `groupId` | the link group of the trade, `!= 0` |
| 2 | 1 | u8 | `seq` | the trade's number in that group (1, 2, ... each member counts its own READYs), `!= 0` |
| 3 | 1 | u8 | `reason` | ABORT: 1 GROUP_ENDED, 2 TIMEOUT, 3 MISMATCH; READY / COMMIT: 0. Anything else rejected |
| 4 | 4 | u32 | `tradeId` | READY: the journal's trade id (6.16), `!= 0`; the host aborts both (MISMATCH) if the two READYs of a round differ, and remembers its decision per trade id. COMMIT / ABORT: 0 (ignored) |

**Member.** Once its save is written except for the signature, it sends READY(groupId, seq) and
waits. The game keeps link errors from leaving the trade screen while it waits
(`SetSuppressLinkErrorMessage`), because the decision arrives over the session even if the
cable dies. COMMIT → write the signature. ABORT → do not; the link-error path reloads the
previous save (PS-048). If the member's own session ends while it waits, it presumes ABORT
(local reason IN_DOUBT 33). The host's own member uses direct calls, not the wire.

**Host (the arbiter).** Per slot it records the latest round (groupId, seq, state):
- READY that does not match the sender's current link group (no group, another id, not exactly
  2 members) → ABORT MISMATCH.
- READY from both members of the group for the same (groupId, seq) → COMMIT to both. When the
  host is one of them, the client's COMMIT is sent and flushed (`MpTransport_Flush`) before the
  host's own game sees its COMMIT.
- Both members waiting for different `seq` → ABORT MISMATCH to both.
- The group ends (a member left the link or the session, or joined another group) while a
  member waits → ABORT GROUP_ENDED; a round not complete `MP_TRADE_COMMIT_TIMEOUT_MS` (15 s)
  after its first READY → ABORT TIMEOUT. In both cases the partner's late READY for that round
  gets the same ABORT.
- A decision is final: a repeated READY for a decided round gets the same answer again.

Malformed (parser failure, COMMIT / ABORT sent to the host, READY sent to a client): dropped and
counted against the 8.2 budget. A decision for a round the member is not waiting for is ignored.

**Guarantee.** Both saves change only through a COMMIT, and the host sends COMMIT only after it
holds READY from both partners, at which point both have the received Pokémon written and only
the signature left. So:
- Anything that goes wrong before both READYs have reached the host (a member's game, link,
  session or process ends; the link errors; a timeout) leaves **both saves unchanged**: nobody
  can get COMMIT for that round any more.
- Once both have received COMMIT, both sign: the trade is in both saves. A link error after that
  reloads the new save.
- **Remaining window (in doubt).** A member that has sent READY and loses its connection to the
  host before COMMIT reaches it cannot know whether the host decided COMMIT. It presumes ABORT.
  If the host did commit (both READYs had arrived), the other side signs and this side does not:
  one Pokémon is then in both saves (duplicated) and the other in neither (lost). The window is
  about one round trip from the moment the second READY reaches the host, on the side whose
  connection drops: a client's network or process dying in that interval, or the host process
  dying between flushing COMMIT and signing its own save (one frame). **PS-050 closes it** with
  the trade journal (6.16): the in-doubt side keeps a READY journal and resolves it later.

### 6.16 `TRADE_RECOVER` (0x6D) — both directions, channel 0, payload 32 bytes (PS-050)

**The journal** (`<save>.trade`, written crash-safe like the save: tmp + sync + atomic
replace; layout in `src/mp/mp_trade_journal.c`, 156 bytes with magic, version and FNV-1a
checksum). It holds the trade id, both trainer ids, the partner's name, both mons' identity
(personality, OT id, species), our party slot, and the received mon's 100 bytes (after any
trade evolution). States and the order of durable writes per side:

1. **READY** is written before READY is sent. The save on disk is the pre-trade one.
2. On COMMIT, **COMMIT** is written *before* the signature. Then the save is signed and
   stored (BUG-003: `LinkFullSave_SetLastSectorSignature` writes the file).
3. A final ABORT deletes the journal. An IN_DOUBT ABORT keeps it READY.
4. A COMMIT journal is deleted once the partner says it knows (COMMITTED / NONE_HAS / NONE_HASNOT).

`tradeId` = FNV-1a over (trainer id, given personality, given OT id) of both sides, the lower
trainer id first (never 0). Both sides compute it alike before READY. A pending journal blocks
new trades.

| Offset | Size | Type | Field | Rule |
|---|---|---|---|---|
| 0 | 1 | u8 | `slot` | C → H: target slot, or `0xFF` (ask the host only). H → C: the sender's slot, or `0xFF` (the host's own decision) |
| 1 | 1 | u8 | `status` | 1 READY, 2 COMMITTED, 3 NONE_HAS, 4 NONE_HASNOT, 5 BUSY, 6 DAMAGED, 7 ABORTED (host only) |
| 2 | 1 | u8 | `flags` | bit 0: please reply; other bits 0 |
| 3 | 1 | u8 | reserved | 0 |
| 4 | 4 | u32 | `tradeId` | `!= 0` |
| 8 | 4 | u32 | `senderTid` | the sender's full trainer id |
| 12 | 4 | u32 | `receiverTid` | |
| 16 | 8 | u32×2 | sender's given mon | personality, OT id |
| 24 | 8 | u32×2 | sender's received mon | personality, OT id |

**Validation (AD-9).**
- The host drops messages whose `senderTid` isn't the sender's session trainer id. It routes
  only to a target whose trainer id is `receiverTid`.
- The receiver requires:
  - `tradeId` == the id its fields give;
  - `receiverTid` == its own;
  - for a relayed message, that the sending slot's trainer id is `senderTid`;
  - for any action on its journal, an exact match of the trade id, both trainer ids and both mons.
- Nothing from the network is ever applied as a Pokémon. Rolling forward uses only the mon
  stored in our own journal (validated with `MP_ValidateMon` on load). A journal file is parsed
  as untrusted input and must belong to this save's trainer id. A damaged journal is kept and
  shown, and is never guessed about.

**Statuses** (what a side reports about a trade):
- READY / COMMITTED: from its journal. BUSY while that trade is still live.
- Without a journal for it: NONE_HAS if its party or boxes contain the mon the asker gave,
  else NONE_HASNOT.

**Rules** (`MpTradeJournal_Resolve`). A signed save always means the outcome is COMMIT, so a
save is only ever rolled **forward**:

| My journal | Other side says | Action |
|---|---|---|
| READY | COMMITTED, NONE_HAS | write COMMIT, then apply the trade in the field (replace the given mon, found by identity in the party or boxes, with the stored one; Pokédex seen/caught; trade stat) and save |
| READY | READY, NONE_HASNOT, ABORTED (host) | delete: nobody signed, and nobody can learn COMMIT any more |
| COMMIT | COMMITTED, NONE_HAS, NONE_HASNOT | delete (once applied) |
| any | BUSY, DAMAGED, other | keep |

**Routing.**
- Pending sides send their status every ~2 s with "reply". They send to the partner's slot
  when it is in the session (found by trainer id), else to the host only.
- The host first sends its remembered decision for that trade id (a ring of its last 8
  COMMIT/ABORT decisions, keyed by READY's `tradeId`; READYs outside a real group are not
  recorded) to the sender *and* the target. Only then does it relay the message.
- Since the ring only ever loses entries, everyone hears the host's decision before any
  partner status about that trade while the host still remembers it. So the READY + READY →
  delete rule can't race a COMMIT learned from the host.

**Guarantee.** Every combination of a crash at each step (before READY, after READY, after
COMMIT and before signing, after signing and before the journal is deleted), on the client or
the host, with in-flight decisions delivered or lost, ends with both saves traded or both not,
and no journal left. This is checked by `MpSelftest_Trade`'s crash simulation over the real rules.
- If the partner never comes back, the journal stays and is shown ("A trade with NAME isn't
  finished"). It is only resolved early by the host's remembered decision.
- Residual risks:
  - the player moves or releases the traded mon before resolution, so the mon can't be found:
    the journal is kept as damaged;
  - a peer that lies (outside the friends-only trust model).

---

### 6.17 `RESUME` (0x0A) — client → host, channel 0, payload 40 bytes (PS-054)

The first message on a **new** ENet connection of a client that lost its connection (7.6), in
place of `HELLO`. HELLO's layout, frozen prefix included, with the held slot and epoch in bytes
13–14 and the token from the last WELCOME appended:

| Offset | Size | Type | Field | Value |
|---|---|---|---|---|
| 0 | 12 | | frozen prefix | as HELLO: `magic`, `protocolVersion`, reserved, `buildHash` |
| 12 | 1 | u8 | `buildFlags` | as HELLO |
| 13 | 1 | u8 | `slot` | the slot the client held, 1..3 (else BAD_FIELD) |
| 14 | 1 | u8 | `epoch` | its epoch, `!= 0` |
| 15 | 1 | u8 | reserved | 0 |
| 16 | 16 | PlayerSummary | `summary` | the client's current summary (5.1) |
| 32 | 8 | u8[8] | `token` | from the last WELCOME; not all zero (else BAD_FIELD) |

**Host processing** (the peer must be pending, as for HELLO; a RESUME from a peer already in the
table is a second HELLO: `BYE(PROTOCOL_ERROR)`):

1. HELLO steps 2–4 (prefix, VERSION, BUILD) with the same REJECTs.
2. Exact size, fields and summary → else `REJECT(BAD_DATA)`.
3. `slot` is occupied, its epoch equals `epoch`, and `token` equals the token the host last gave
   that slot → else `REJECT(RESUME_REFUSED)`. A token works **once**: the WELCOME that answers it
   carries a new one, so a replayed RESUME is refused. The token is forgotten when the slot is freed.
4. The summary's trainer id, name and gender equal the slot's → else `REJECT(RESUME_MISMATCH)`
   (the right token from a different save; badges may differ). A refused RESUME changes nothing.
5. Accept: if the slot still has a peer (the client noticed the dead link first), that peer is
   reset and the slot's interaction requests and link group end, as at a loss (7.6). The slot is
   bound to the new peer, `connectionLost` is cleared and the summary stored, then in this order:
   1. `WELCOME` with `RESUMED`, a new token, the full table (lost slots flagged);
   2. the cached `PRESENCE` of every other player (channel 1);
   3. if the slot was held: `PLAYER_STATUS(ONLINE)` to every other client, and
      `MP_EVENT_PLAYER_RECONNECTED` locally;
   4. if the summary changed while away: `PLAYER_UPDATE` to every other client.

### 6.18 `PLAYER_STATUS` (0x0B) — host → client, channel 0, payload 4 bytes (PS-054)

| Offset | Size | Type | Field | Validation (client) |
|---|---|---|---|---|
| 0 | 1 | u8 | `slot` | 1..3, a remote slot (not ours) |
| 1 | 1 | u8 | `epoch` | `!= 0`; a stale epoch is ignored |
| 2 | 1 | u8 | `status` | `0` ONLINE, `1` LOST (else BAD_FIELD) |
| 3 | 1 | u8 | reserved | ignored |

LOST: the host lost that player's connection and holds its slot (7.6); the client sets
`MpPlayer.connectionLost`, ends its own requests with that player and queues
`MP_EVENT_PLAYER_RECONNECTING`. ONLINE: the player is back (`MP_EVENT_PLAYER_RECONNECTED`). A
status equal to the current one is ignored.

## 7. Session rules

### 7.1 Connect sequence

```
Client                                   Host
MP_Join(ip, port)  -> CONNECTING
  enet connect (data = MP_CONNECT_MAGIC) ---->  ENet CONNECT: check magic, start 5 s HELLO timer
  ENet CONNECT  <----------------------------
  HELLO -------------------------------------->  validate (6.1)
                                                 ok:   WELCOME ------------------>  IN_SESSION
                                                       PLAYER_JOINED -> others
                                                       cached PRESENCE x(N-1) ---> (ch 1)
                                                 fail: REJECT(reason) ----------->  ERROR
                                                       disconnect_later
```

The client's 5000 ms connect timer runs from `MP_Join()` to WELCOME/REJECT. On expiry: reset the
peer, state ERROR, `MP_EVENT_CONNECT_FAILED(MP_REASON_TIMEOUT)`.

### 7.2 Leaving

- **Client `MP_Leave()`:** send `BYE(LEAVING)`, `enet_peer_disconnect_later`, state OFFLINE
  immediately. The transport keeps servicing in later `MP_Update()` calls until the ENet
  disconnect completes or `MP_LEAVE_FLUSH_MS` (1000 ms) passes, then resets the peer and destroys
  the ENet host.
- **Host `MP_Leave()`:** send `BYE(HOST_SHUTDOWN)` to every peer, `disconnect_later` all, state
  OFFLINE immediately, flush as above.
- **`MP_Shutdown()`** (game quitting): same as `MP_Leave()`, but services the host synchronously
  for at most 250 ms so the BYE has a chance to go out, then destroys everything.
- A client whose host disappears keeps playing offline on its own save (AD-1, AD-10).

### 7.3 Timeouts

| Timer | Value | Who | Action on expiry |
|---|---|---|---|
| Connect / handshake | 5000 ms (`MP_CONNECT_TIMEOUT_MS`) | client: `MP_Join` → WELCOME/REJECT. Host: ENet connect → HELLO. | client: CONNECT_FAILED(TIMEOUT). Host: reset peer silently. |
| Idle | 10000 ms (`MP_IDLE_TIMEOUT_MS`) | every peer, time since **any** packet was received from that peer | PS-054: a lost connection (7.6). Host: hold the slot. Client: reconnect. |
| ENet peer timeout | **The session must set this explicitly** on every peer as soon as its ENet connection is established: `enet_peer_timeout(peer, 0, 4000, 7000)` and `enet_peer_ping_interval(peer, 500)` (limit 0 = ENet default retry count, minimum 4000 ms, maximum 7000 ms; PS-054, was 5000 / 10000). ENet's defaults (up to 30 s) are too long. | ENet | surfaces as an ENet DISCONNECT event: a lost connection (7.6) |
| Reconnect grace | 35000 ms (`MP_RECONNECT_GRACE_MS`, PS-054) | host, per lost slot, from the loss | free the slot: `PLAYER_LEFT(CONNECTION_LOST)` as before PS-054 |
| Reconnect window | 30000 ms (`MP_RECONNECT_WINDOW_MS`, PS-054) | client, from the loss | give up: state ERROR, `DISCONNECTED(CONNECTION_LOST)` |
| Reconnect attempt | 3000 ms; waits 250, 500, 1000, 2000, 2000 … ms between attempts | client | reset the attempt's peer, wait, dial again |
| Leave flush | 1000 ms (`MP_LEAVE_FLUSH_MS`) | after `MP_Leave()` | reset remaining peers, destroy ENet host |
| Ping interval | 1000 ms | host ↔ each client | send PING |
| Presence keep-alive | 1000 ms | every IN_SESSION player | resend current presence (new `seq`) |

**Acceptance tolerance:** a peer that vanishes without BYE (process killed, cable pulled) must be
detected within **15 s** of its last packet (nominally 4–7 s). Since PS-054 detection starts the
reconnect logic of 7.6: the host reports `PLAYER_STATUS(LOST)` at once and `PLAYER_LEFT` only after
the 35 s grace; a client reports `DISCONNECTED` only after its 30 s reconnect window.

All timers use `MpTransport_GetTimeMs()` (wraps `enet_time_get()`, milliseconds, u32) with
wrap-safe `(u32)(now - start) >= timeout` comparisons. They are wall-clock, so pausing (Ctrl+P)
or speed-up does not cause timeouts. The network is still pumped while the game is paused.

### 7.4 Slots and epochs

- Slot 0 is the host. Clients get the lowest free slot in 1..3.
- The host keeps a u8 epoch counter starting at 1. Every slot assignment (including the host's own
  slot 0 at `MP_Host()`) takes the next value; the counter skips 0 on wrap.
- Every slot-scoped message carries the epoch. A message whose epoch does not match the receiver's
  table for that slot refers to a previous occupant and is ignored. This makes immediate slot
  reuse safe even though PRESENCE (channel 1) and PLAYER_LEFT/JOINED (channel 0) are not ordered
  relative to each other.

### 7.5 What the host relays

| Received from client | Host action |
|---|---|
| `PRESENCE` | validate, cache, forward to all other clients |
| `PLAYER_UPDATE` | validate, store, forward to all other clients |
| `PING` | answer `PONG` (not forwarded) |
| `INTERACT_*` | route to the other party after the checks of 6.11 (or answer / handle it itself) |
| `TRAINER_CARD_DATA` | forward to the requester along the matching TRAINER_CARD route (6.13), at most one per route |
| `PARTY_DATA` | forward to the requester along the matching VIEW_PARTY route (6.14), in order, at most two per route |
| `BYE` | remove client, broadcast `PLAYER_LEFT` |
| `RESUME` (pending peer only) | 6.17: give the held slot back, or REJECT (PS-054) |
| `LINK_JOIN` `LINK_LEAVE` `LINK_OPEN` `LINK_CLOSE` `LINK_START` `LINK_CMD` | link bus (6.12): not forwarded as such; the host answers with `LINK_GROUP` / `END` / `STATUS` / `BUS_START` / `BUS_END` / `ROW` |
| `TRADE_COMMIT` READY | trade commit arbiter (6.15): not forwarded; the host answers `TRADE_COMMIT` COMMIT / ABORT to the members |
| `TRADE_RECOVER` | journal recovery (6.16): its own remembered decision first (to sender and target), then routed to the target with `slot` = the sender |
| anything else | drop, count malformed |

The host's own presence and summary changes are sent by the host to every client directly
(slot 0).

### 7.6 Dropouts and reconnect (PS-054)

A connection is **lost** when ENet reports a DISCONNECT for a peer in the player table, or the idle
timer expires. A `BYE`, a kick or a REJECT is not a loss and ends the session as before.

**Host**, when it loses a client:

1. The slot's interaction requests end (6.11: requesters get UNAVAILABLE, targets CANCELLED) and
   its link group ends with `LINK_END(LOST)` for the other members, exactly as if it had left: the
   partners' PS-048 link-error recovery runs, an undecided `TRADE_COMMIT` round is aborted, and the
   host's remembered decisions (6.15, 6.16) are kept.
2. The slot, name, epoch, summary, token and cached presence are **held**; the slot has no peer.
   `PLAYER_STATUS(LOST)` goes to every other client; locally `MP_EVENT_PLAYER_RECONNECTING`.
   Requests to the slot are answered UNAVAILABLE and link groups with it are refused.
3. A valid `RESUME` (6.17) within `MP_RECONNECT_GRACE_MS` gives the slot back.
4. Otherwise, after the grace, the slot is freed with `PLAYER_LEFT(CONNECTION_LOST)` as before.
5. A `HELLO` whose trainer id, name and gender equal a held slot's (the player restarted the game)
   frees the held slot first (`PLAYER_LEFT(CONNECTION_LOST)`), then joins normally.

**Client**, when it loses the host while IN_SESSION (and holds a token):

1. It stays IN_SESSION with its table; `MP_IsReconnecting()` is TRUE; `MP_EVENT_RECONNECTING` is
   queued. Its link group ends locally (`MP_LINK_END_SESSION`, the PS-048 path); a trade waiting
   for its commit presumes ABORT (IN_DOUBT) and its PS-050 journal stays; interaction requests end.
   It sends nothing else (no PRESENCE, PING, requests or link joins) until it is back.
2. It re-dials the host from the same ENet host (`MpTransport_Reconnect`): one attempt = ENet
   connect + `RESUME` + wait for the answer, at most 3000 ms; between attempts it waits 250 ms,
   doubling to 2000 ms.
3. `WELCOME(RESUMED)` for its own slot and epoch: back. The table is merged: players that are still
   there (same epoch) keep their presence; differences are reported as `PLAYER_JOINED` / `LEFT` /
   `UPDATED` / `RECONNECTING` / `RECONNECTED` events. The new token replaces the old one;
   `MP_EVENT_RECONNECTED`; the current PRESENCE is sent. A WELCOME for another slot or epoch is a
   protocol error.
4. `REJECT` (any reason), or `MP_RECONNECT_WINDOW_MS` over: state ERROR,
   `DISCONNECTED(CONNECTION_LOST)` (as before PS-054).

The grace (35 s) is longer than the window (30 s), so a client never comes back to a slot the host
has just freed. Short losses of about 2 s never reach this: ENet retransmits and nothing is lost.

**Trade journal and link-error recovery.** Nothing new is needed: a loss ends the link activity
exactly like a player leaving. If the host had already sent COMMIT for the round, the other member
signs and the lost member (IN_DOUBT) keeps its READY journal; after the reconnect its journal's
`TRADE_RECOVER` gets the host's remembered COMMIT and rolls forward. If not, both abort and the
host answers ABORTED. If the client never comes back, its journal stays until it meets the partner
or host again, as before.

---

## 8. Validation (AD-9)

### 8.1 Rules applied to every packet

1. Size and header checks of section 3. Failure → drop.
2. Type valid for the receiver's role, state and the channel it arrived on (section 4). Failure →
   drop.
3. Exact payload size for the type (`WELCOME`: `4 + 20*N` with N checked first). Failure → drop.
4. Parse with the bounded `MpReader` (sticky error flag: any read past the end sets `error` and
   returns 0). A message is applied **only** if parsing finished with `error == FALSE` **and** every
   field rule passed. Parse into a local temporary; copy into the player table only after full
   validation. No partial application.
5. Every enum and index is range-checked **before** it is used as an array index.
6. Names follow 5.1.1 and are always stored with `EOS` padding; nothing ever `memcpy`s a remote
   buffer of remote-controlled length.
7. Fields from a client that the host is authoritative for (`slot`, `epoch`, `entryFlags`) are
   overwritten, never trusted.
8. The receive data pointer is only valid inside the transport callback; nothing keeps it.

### 8.2 Malformed-message budget

- Host, per client peer: each dropped message counts 1 (except silent stale/epoch drops and
  rate-limit drops). At **8**, send `BYE(PROTOCOL_ERROR)`, disconnect, and broadcast
  `PLAYER_LEFT(MP_REASON_KICKED)`.
- Host, pending (pre-HELLO) peer: any message other than a well-formed HELLO → disconnect at once
  (`REJECT(BAD_DATA)` if it was type 0x01, otherwise silent reset).
- Client: a malformed `WELCOME` fails the connection immediately (6.2). Other malformed messages
  from the host are dropped and counted; at **8**, send `BYE(PROTOCOL_ERROR)`, disconnect,
  `MP_EVENT_DISCONNECTED(MP_REASON_PROTOCOL_ERROR)`.
- Every drop is logged with `[MP]` under `--mp-debug` (type, peer, reason); a summary counter is
  always logged when a peer is dropped.

### 8.3 Parser testability (Director ruling, 2026-11-06)

Raw UDP junk sent at the port never reaches our parsers: ENet discards it at the protocol level.
So "100 KB of random UDP" (PS-012) only tests ENet. To test **our** validation, every message
parser is a **pure function** and is fuzzed directly by `--mp-selftest`.

**Parser contract** (`src/mp/mp_protocol.c`, declared in `include/mp/mp_protocol.h`):

```
// Header: checks section 3 (sizes, length field). Does not check type/state/channel.
bool8 MpProto_ParseHeader(const u8 *buf, u32 len, struct MpMsgHeader *out);
// One parser per message. `payload`/`len` = the bytes after the header.
// Returns MP_PARSE_OK, or an MP_PARSE_ERR_* code (TRUNCATED, OVERSIZED, BAD_FIELD, BAD_NAME,
// BAD_MAGIC, ...). On error, *out is zeroed and must not be used.
u8 MpProto_ParseHello(const u8 *payload, u32 len, struct MpMsgHello *out);
u8 MpProto_ParseWelcome(const u8 *payload, u32 len, struct MpMsgWelcome *out);
u8 MpProto_ParseReject(const u8 *payload, u32 len, struct MpMsgReject *out);
u8 MpProto_ParsePlayerEntry(const u8 *payload, u32 len, struct MpMsgPlayerEntry *out); // JOINED, UPDATE
u8 MpProto_ParsePlayerLeft(const u8 *payload, u32 len, struct MpMsgPlayerLeft *out);
u8 MpProto_ParseBye(const u8 *payload, u32 len, struct MpMsgBye *out);
u8 MpProto_ParsePing(const u8 *payload, u32 len, struct MpMsgPing *out);           // PING, PONG
u8 MpProto_ParsePresence(const u8 *payload, u32 len, struct MpMsgPresence *out);
// Writers: return bytes written (header included), or 0 if `cap` is too small.
u16 MpProto_WriteHello(u8 *buf, u16 cap, const struct MpMsgHello *msg);
// ... one writer per message, same shape.
```

Rules: parsers take **no global state** (no player table, no session state, no time) and do no
I/O and no allocation. They check everything that can be checked from the bytes alone: sizes,
magic, enum ranges, name rules, WELCOME internal consistency. Checks that need session state
(slot occupied? epoch matches? newer seq? `MP_IsValidMap`? rate limits?) are done by the session
code **after** a successful parse. The one exception is `MP_IsValidMap()` (a pure function over
a constant table), which `MpProto_ParsePresence` calls.

**Selftest fuzzing** (`--mp-selftest`, extended by PS-012 for the parsers and by PS-013 for the
summary). For **every** message type:

1. **Round trip:** a valid message written then parsed gives identical fields.
2. **Truncated:** the valid encoding cut to every length `0 .. n-1` → error, no crash.
3. **Oversized:** the valid encoding plus 1 byte, and padded to 512 and 513 bytes → error.
4. **Bit flips:** each single-bit flip of the valid encoding → either OK (if the field allows it)
   or an error, never a crash. The result must be deterministic.
5. **Garbage:** at least 100 000 buffers of random length 0..600 and random content, from a fixed
   seed (logged, so failures reproduce) → no crash. Run under AddressSanitizer
   (`-fsanitize=address,undefined`) on Linux at least once before PS-012 hands off.
6. **Targeted cases:** name with no EOS, name with EOS at byte 0, name with 0xFC/0xFD before EOS,
   gender 2, `WELCOME` with N = 0, N = 5, duplicate slots, no slot 0, missing `yourSlot`,
   `PRESENCE` with `mapGroup = 0xFF, mapNum = 0`, facing 0 and 5.

The selftest prints one line per group and `[MP] selftest OK` / `[MP] selftest FAILED (<n>)`, and
exits with status 0 / 1.

### 8.4 Rate limits

| Traffic | Sender rule | Receiver (host) enforcement |
|---|---|---|
| `PRESENCE` | Send on change, at most 20 Hz (>= 50 ms apart). A new `stepId` always forces a send as soon as the 50 ms window allows (several steps inside one window merge into the newest state; receivers see the `stepId` gap). Keep-alive every 1000 ms when unchanged. | drop if < 25 ms since the last accepted one from that client (not counted malformed) |
| `PLAYER_UPDATE` | at most 1 / s, coalesced | drop extras within 1 s |
| `INTERACT_REQUEST` | one in flight per player (the API refuses a second) | refuse (`UNAVAILABLE`) a second one in flight, or one < 1000 ms after the previous; not counted malformed |
| `TRAINER_CARD_DATA` | one per TRAINER_CARD request it answers | one per route; a second one (or one on a route of another kind) is malformed |
| `PARTY_DATA` | 1–2 parts per accepted VIEW_PARTY request | in order, at most 2 per route; otherwise malformed |
| `PING` | 1 / s | ignore PINGs < 250 ms apart |
| `HELLO` | once per connection | a second one → disconnect |
| ENet events | — | at most 64 processed per `MP_Update()` |

**Bandwidth estimate (PS-030 acceptance: < 2 KB/s per player while walking).** PRESENCE is 20 bytes
of message + about 12 bytes of ENet headers + 28 bytes UDP/IPv4 ≈ 60 bytes. At the 20 Hz maximum
that is ≈ 1.2 KB/s up per client. The host sends each client the other players' presence, so
host upload ≈ 1.2 KB/s × 3 × 3 ≈ 11 KB/s worst case with 4 players. PING/PONG add < 0.2 KB/s.

---

## 9. Build hash and build id

The build hash identifies the game build so mismatched builds are rejected (AD-7) with
`REJECT(BUILD)`. Per the Director ruling of 2026-11-06 it is derived from **the protocol version
and the git short hash only**. It never includes OS, bitness, compiler or build flags: 64-bit
Windows ↔ Linux cross-play is required for M1, and 32-bit builds of the same commit must also
match (32-bit support is best effort).

### 9.1 Build id string

`MP_BUILD_ID` is a string injected at build time with `-DMP_BUILD_ID=\"…\"`, only on
`src/mp/mp_build.c` (see `design.md` §4.3):

| Situation | `MP_BUILD_ID` | `MP_BUILD_DIRTY` |
|---|---|---|
| Normal git checkout | first 8 hex digits of `git rev-parse HEAD`, lowercase (e.g. `1a2b3c4d`) | `0` |
| Uncommitted changes to tracked files (`git status --porcelain --untracked-files=no` non-empty) | the same 8 hex digits (**no** `-dirty` suffix) | `1` |
| No git, or not a git checkout (source tarball) | `nogit` | `0` |
| `make linux MP_BUILD_ID=<str>` override | `<str>` (max 15 chars) | as detected |

The dirty flag is sent in `HELLO.buildFlags` bit 0 and logged by the host ("client is a dirty
build"), but it is **not** part of the hash. Two developers on the same commit can therefore play
together while editing; desyncs caused by their uncommitted changes are their own responsibility.
`nogit` builds match only other `nogit` builds of the same protocol version.

### 9.2 Hash

```
MP_GetBuildHash() = FNV-1a-32( [ver & 0xFF, ver >> 8] ++ ascii(buildId) )
   ver     = MP_PROTOCOL_VERSION (or the --mp-fake-protocol value)
   buildId = MP_BUILD_ID          (or the --mp-fake-build value), without its NUL
FNV-1a-32: h = 0x811C9DC5; for each byte b: h = (h ^ b) * 0x01000193 (mod 2^32)
```

It is computed at `MP_Init()` and again whenever an override is set. Because the protocol
version is folded in, `HELLO.protocolVersion` is still compared **first**, so the UI says VERSION
rather than BUILD when that is the real cause.

### 9.3 Test overrides (debug flags, Director ruling 2026-11-06)

| Flag | API | Effect |
|---|---|---|
| `--mp-fake-protocol <n>` (0..65535) | `MP_SetFakeProtocolVersion(n)` | This instance advertises and expects version `n`. It is written to `HELLO.protocolVersion` **and** the HELLO header `version` byte (low 8 bits), used by the host's step-3 check, written to `REJECT.hostProtocolVersion`, and folded into the build hash. All other messages keep the real version byte. Run one instance with `--mp-fake-protocol 99` to test `REJECT(VERSION)`. |
| `--mp-fake-build <str>` (1..15 printable ASCII chars) | `MP_SetFakeBuildId(str)` | The build hash is computed from `<str>` instead of `MP_BUILD_ID`. Run one instance with `--mp-fake-build test` to test `REJECT(BUILD)` without building a second binary. |

Both are logged loudly at startup (`[MP] WARNING: fake protocol version 99 in use`). They are
for testing only. They are harmless in release builds because they can only make *this* instance
fail to connect.

---

## 10. Worked example: HELLO

Client "ASH" (game charset `BB CD C2`), male, badges 1–3, TID 12345 (0x3039), SID 5, clean tree.
The build hash `0x12345678` is an illustrative value (a real one is an FNV-1a result, 9.2).
Packet = 36 bytes:

```
01 0B 20 00                 header: type HELLO, version 11, length 32
50 53 4D 50                 magic "PSMP"
0B 00                       protocolVersion 11
00 00                       reserved
78 56 34 12                 buildHash 0x12345678
00                          buildFlags
00 00 00                    reserved
BB CD C2 FF FF FF FF FF     name "ASH" + EOS padding
00                          gender MALE
00                          avatarStyle PLAYER
07                          badges 0b00000111
00                          summaryFlags
39 30 05 00                 trainerId 0x00053039
```

---

## 11. Change log

| Version | Card | Change |
|---|---|---|
| 1 | PS-010 | Initial spec |
| 1 | PS-030 | PRESENCE implemented. No byte change, so no version bump: field semantics pinned down (6.8.1); a CONNECTING client drops PRESENCE silently instead of counting it malformed (a relayed update can overtake a retransmitted WELCOME; it was dropped before too, only the malformed count changes); `MP_IsValidMap` from a compile-time-checked map count table. |
| 2 | PS-041 | `INTERACT_REQUEST` (0x40), `INTERACT_RESPONSE` (0x41), `INTERACT_CANCEL` (0x42): trade / battle requests routed by the host (6.11), with host-side checks (same map, one request per requester, 1 s gap, busy target) and timeouts. Version bumped to 2: a version-1 peer is rejected (VERSION) instead of meeting unknown message types. |
| 3 | PS-046 | Virtual link cable: `LINK_JOIN` … `LINK_ROW` (`0x60`–`0x6B`, 6.12) on channel 0, host-sequenced rows, link groups, bus epochs. Wire change, so `MP_PROTOCOL_VERSION` 2 → 3 (older builds are rejected with VERSION). Worked example (10) updated. |
| 4 | PS-042 | INTERACT `kind` 3 TRAINER_CARD (answered by the target's session, no prompt, never busy) and `TRAINER_CARD_DATA` (0x43, 56 bytes, field by field, validated and clamped; 6.13), routed like the RESPONSE it precedes, one per route. Local result `MP_INTERACT_RESULT_BAD_DATA`. Version bumped to 3: a version-2 peer would reject kind 3 as malformed. *(If PS-046's `LINK_*` change lands first and takes 3, this becomes the next number.)* |
| 5 | PS-043 | INTERACT `kind` 4 VIEW_PARTY (prompted, like a trade) and `PARTY_DATA` (0x44): the party as raw 100-byte `struct Pokemon`s in 1–2 parts of up to 3 (a full party does not fit in one 512-byte packet), sent before the ACCEPTED RESPONSE, forwarded in order along the route; every mon validated by the requester (6.14.1), any bad mon rejects the whole party (BAD_DATA). *(Renumber after PS-046 as above.)* |
| 6 | PS-044 | `TRADE_COMMIT` (0x6C, 6.15) in the `LINK_*` range: the host-arbitrated commit of a direct trade's save (READY / COMMIT / ABORT). Version bumped to 6: a version-5 peer would count `0x6C` as malformed. Worked example (10) updated to version 6. |
| 7 | PS-050 | Trade journal recovery: `TRADE_COMMIT` grows to 8 bytes (`tradeId`), new `TRADE_RECOVER` (0x6D, 32 bytes, 6.16). The post-trade save is written to disk at the signature (BUG-003). Version bumped to 7. |
| 8 | PS-045 | INTERACT `kind` 5 BATTLE_DOUBLE (6.11): a battle request that asks for a DOUBLE link battle; `kind` 2 BATTLE is now the single link battle. Prompted, routed and answered exactly like BATTLE. Version bumped to 8: a version-7 peer rejects kind 5 as malformed. No new message: the battle itself runs over the PS-046 cable (`LINK_*`) unchanged. Worked example (10) updated to version 8. |
| 9 | PS-051 | `LINK_DONE` (0x6E, 6.12): the agreed end of a link activity. A member that is done sends it, then closes its cable (no close-link handshake). The host relays the done mask, ends the group with the new `END` reason 5 DONE once all are done (or 3 s after the first), and emits queued rows before it handles `DONE` / `CLOSE`. A finishing member does not treat `BUS_END` or the group ending as a hardware error (fixes BUG-007). Version bumped to 9: a version-8 peer counts `0x6E` as malformed. Worked example (10) updated to version 9. |
| 10 | PS-052 | `LINK_BLOCK` (0x6F, 6.12 "Whole blocks"): a `SendBlock` of 1..500 bytes travels as one message instead of its `INIT_BLOCK` + `CONT_BLOCK` rows; the host emits the queued rows, then relays the block to every member with the sender's link id (one sequenced stream per group). Bigger blocks keep the rows. Row pacing plays backlogged block-data rows up to 8 per tick (no wire change). Version bumped to 10: a version-9 peer counts `0x6F` as malformed. Worked example (10) updated to version 10. |
| 11 | PS-054 | Survive dropouts (7.6). `WELCOME` grows to 12 + 20·N: byte 3 is `flags` (bit 0 RESUMED), bytes 4–11 a random reconnect `token` (never zero). New `RESUME` (0x0A, 40 bytes, 6.17: HELLO's layout + slot, epoch, token) and `PLAYER_STATUS` (0x0B, 4 bytes, 6.18: a player's connection LOST / ONLINE). `PlayerEntry` flag bit 1 LOST (WELCOME only). REJECT reasons 5 RESUME_REFUSED and 6 RESUME_MISMATCH. A lost connection (ENet timeout, now 4000 / 7000 ms, or idle) no longer frees the slot at once: the host holds it for 35 s, the client retries for 30 s. Host peer count 6. Version bumped to 11: a version-10 peer would read the token as player entries. Worked example (10) updated to version 11. |
