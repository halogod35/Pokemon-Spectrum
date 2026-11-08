# Pokémon Spectrum — Multiplayer Wire Protocol

- **Card:** PS-010 (spec), implemented by PS-011 (transport), PS-012 (session), PS-013 (summary), PS-030 (presence), PS-041 (interaction requests), PS-046 (virtual link cable)
- **Protocol version:** `MP_PROTOCOL_VERSION = 3` (2 = `INTERACT_*`, PS-041; 3 = `LINK_*`, PS-046)
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
| Host peer count | `MP_MAX_PLAYERS` (= 4). 3 for clients + **1 spare**, so a 5th player can connect at the ENet level and receive `REJECT(FULL)` instead of a silent ENet refusal. |
| Client peer count | 1 |
| Bandwidth limits | 0 / 0 (unlimited; our traffic is tiny) |
| Compression | none |
| `host->maximumPacketSize` | `MP_MAX_PACKET_SIZE` = **512** (ENet ≥ 1.3.13 has this field; it defaults to 32 MB, which is a memory DoS) |
| `host->maximumWaitingData` | 16384 |
| `enet_peer_timeout(peer, 0, 5000, 10000)` | Set on every peer when its ENet connection is established |
| Service | `enet_host_service(host, &ev, 0)` in a loop, on the main thread, once per `MP_Update()`, **at most 64 events per call** (bounds frame time under a flood; the rest are handled next frame) |

### 1.1 Channels

| Channel | ENet flags | Used for |
|---|---|---|
| **0** `MP_CHANNEL_CONTROL` | `ENET_PACKET_FLAG_RELIABLE` | All session/control messages (`0x01`–`0x07`), `INTERACT_*` (6.11) and the virtual link cable (`0x60`–`0x6B`, 6.12) |
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
| `0x02` | `WELCOME` | H → C | 0 | 4 + 20·N (N = 1..4) | client: CONNECTING |
| `0x03` | `REJECT` | H → C | 0 | 8 | client: CONNECTING |
| `0x04` | `PLAYER_JOINED` | H → C | 0 | 20 | client: IN_SESSION |
| `0x05` | `PLAYER_LEFT` | H → C | 0 | 4 | client: IN_SESSION |
| `0x06` | `PLAYER_UPDATE` | C → H, H → C | 0 | 20 | IN_SESSION |
| `0x07` | `BYE` | both | 0 | 4 | any connected state |
| `0x08` | `PING` | both | 1 | 4 | IN_SESSION |
| `0x09` | `PONG` | both | 1 | 4 | IN_SESSION |
| `0x0A`–`0x0F` | reserved: session/control | | 0 | | |
| `0x10` | `PRESENCE` | C → H, H → C | 1 | 16 | IN_SESSION |
| `0x11`–`0x1F` | reserved: presence/world | | 1 | | |
| `0x20`–`0x3F` | reserved: social (chat, emotes) | | | | later |
| `0x40` | `INTERACT_REQUEST` | C → H, H → C | 0 | 8 | IN_SESSION (PS-041, 6.11) |
| `0x41` | `INTERACT_RESPONSE` | C → H, H → C | 0 | 8 | IN_SESSION (PS-041, 6.11) |
| `0x42` | `INTERACT_CANCEL` | C → H, H → C | 0 | 8 | IN_SESSION (PS-041, 6.11) |
| `0x43`–`0x5F` | **reserved: `INTERACT_*`** (Stage 4: trainer card, view Pokémon, …) | | 0 | | Stage 4 |
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
| `0x6C`–`0x7F` | reserved: `LINK_*` | | 0 | | |
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
| 2 | 1 | u8 | `entryFlags` | bit 0 `MP_ENTRY_FLAG_HOST`: set **iff** `slot == 0`. Bits 1–7 reserved (ignored). |
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

### 6.2 `WELCOME` (0x02) — host → client, channel 0, payload 4 + 20·N bytes

| Offset | Size | Type | Field | Validation (client) |
|---|---|---|---|---|
| 0 | 1 | u8 | `yourSlot` | 1..3 |
| 1 | 1 | u8 | `yourEpoch` | `!= 0` |
| 2 | 1 | u8 | `playerCount` N | 1..`MP_MAX_PLAYERS`, and `length == 4 + 20*N` |
| 3 | 1 | u8 | reserved | ignored |
| 4 | 20·N | PlayerEntry[N] | `players` | see below |

The table lists **every** occupied slot including the host (slot 0) and the recipient. Order is
ascending slot, but receivers must not rely on that.

Client validation (any failure → drop the connection with `BYE(PROTOCOL_ERROR)`, state ERROR,
`MP_EVENT_CONNECT_FAILED` with `MP_REASON_PROTOCOL_ERROR`):

- every entry passes 5.2; all `slot` values distinct;
- exactly one entry has slot 0 (and it carries the host flag);
- the entry with `slot == yourSlot` exists and its `epoch == yourEpoch`.

On success: state IN_SESSION, local slot = `yourSlot`, the table is replaced wholesale,
`MP_EVENT_SESSION_STARTED` is queued (no `PLAYER_JOINED` events for players who were already
there), and the client immediately sends its current `PRESENCE`.

### 6.3 `REJECT` (0x03) — host → client, channel 0, payload 8 bytes

**Frozen layout** (never changes across versions; the client parses it whatever the header
`version` is, provided `length >= 8`).

| Offset | Size | Type | Field | Value |
|---|---|---|---|---|
| 0 | 1 | u8 | `reason` | `1` VERSION, `2` BUILD, `3` FULL, `4` BAD_DATA. `5`–`31` reserved. |
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
| 2 | 1 | u8 | `kind` | `1` TRADE, `2` BATTLE. Others rejected |
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

### 6.12 Virtual link cable `LINK_*` (0x60–0x6B) — PS-046, channel 0

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
| `0x63` `LINK_END` | H → C | reason: 1 LEFT, 2 LOST, 3 OVERFLOW, 4 REFUSED (5..31 reserved) | — | reason != 0 |
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

Malformed (parser failure, wrong direction, wrong channel, an unassigned `0x6C`–`0x7F` type):
dropped and counted against the 8.2 budget. Stale (other group, old epoch, old sequence): silent.

---

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
| Idle | 10000 ms (`MP_IDLE_TIMEOUT_MS`) | every peer, time since **any** packet was received from that peer | host: drop client, `PLAYER_LEFT(CONNECTION_LOST)`. Client: `DISCONNECTED(CONNECTION_LOST)`. |
| ENet peer timeout | **The session must set this explicitly** on every peer as soon as its ENet connection is established: `enet_peer_timeout(peer, 0, 5000, 10000)` (limit 0 = ENet default retry count, minimum 5000 ms, maximum 10000 ms). ENet's defaults (up to 30 s) are too long. | ENet | surfaces as an ENet DISCONNECT event, handled as CONNECTION_LOST |
| Leave flush | 1000 ms (`MP_LEAVE_FLUSH_MS`) | after `MP_Leave()` | reset remaining peers, destroy ENet host |
| Ping interval | 1000 ms | host ↔ each client | send PING |
| Presence keep-alive | 1000 ms | every IN_SESSION player | resend current presence (new `seq`) |

**Acceptance tolerance:** a peer that vanishes without BYE (process killed, cable pulled) must be
detected and reported (`PLAYER_LEFT` on the others, or `DISCONNECTED` on a client) within
**15 s** of its last packet. The nominal value is 10 s; the extra 5 s covers ENet's
retransmission back-off and frame timing.

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
| `BYE` | remove client, broadcast `PLAYER_LEFT` |
| `LINK_JOIN` `LINK_LEAVE` `LINK_OPEN` `LINK_CLOSE` `LINK_START` `LINK_CMD` | link bus (6.12): not forwarded as such; the host answers with `LINK_GROUP` / `END` / `STATUS` / `BUS_START` / `BUS_END` / `ROW` |
| anything else | drop, count malformed |

The host's own presence and summary changes are sent by the host to every client directly
(slot 0).

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
01 03 20 00                 header: type HELLO, version 3, length 32
50 53 4D 50                 magic "PSMP"
03 00                       protocolVersion 3
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
