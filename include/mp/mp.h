#ifndef GUARD_MP_MP_H
#define GUARD_MP_MP_H

// Pokemon Spectrum multiplayer: the public API that game code calls.
//
// Contract: docs/multiplayer/design.md and docs/multiplayer/protocol.md (PS-010).
// This is the ONLY multiplayer header that game code (src/*.c, src/platform/*.c)
// may include. It never exposes ENet or socket types.
//
// Part of the PC build only (the GBA build is no longer maintained; Director ruling
// 2026-11-06 overriding AD-6). No #ifdef PORTABLE guards are required, but every
// multiplayer hook in a decomp file (#include of this header, or a call into it)
// must carry a "// MP:" comment so hooks stay greppable:  grep -rn "// MP:" src
//
// Threading: everything here must be called from the main thread only (AD-4).
// Time: all timeouts are measured in wall-clock milliseconds, not game frames,
// so speed-up (timeScale) and pause do not affect them.

#include "gba/types.h"
#include "constants/global.h"

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

#define MP_PROTOCOL_VERSION      1       // Wire protocol version. Bump on ANY wire change. Max 255.
#define MP_BUILD_ID_MAX_LEN      15      // Longest build id string (excluding NUL).
#define MP_MAX_PLAYERS           4       // AD-8. Slot 0 is always the host.
#define MP_MAX_REMOTE_AVATARS    3       // AD-8. Budget for PS-031.
#define MP_DEFAULT_PORT          27500   // AD-3. UDP.
#define MP_SLOT_NONE             0xFF    // "No slot" (not in a session, or invalid).
#define MP_HOST_SLOT             0
#define MP_PLAYER_NAME_SIZE      (PLAYER_NAME_LENGTH + 1) // 8 bytes, game charset, EOS (0xFF) terminated.
#define MP_ADDRESS_MAX_LEN       15      // "255.255.255.255" (IPv4 dotted quad only in M1).

#define MP_CONNECT_TIMEOUT_MS    5000    // MP_Join -> WELCOME/REJECT, and host: connect -> HELLO.
#define MP_IDLE_TIMEOUT_MS       10000   // Nothing received from a peer for this long -> drop it.
#define MP_PING_INTERVAL_MS      1000
#define MP_LEAVE_FLUSH_MS        1000    // How long MP_Leave keeps pumping to deliver BYE.

// Map value used in MpPresence when the player is not in the overworld.
#define MP_MAP_NONE              0xFF

// ---------------------------------------------------------------------------
// Session state
// ---------------------------------------------------------------------------

enum
{
    MP_STATE_OFFLINE,       // No session. Single-player. Default.
    MP_STATE_CONNECTING,    // Client: MP_Join called, waiting for WELCOME.
    MP_STATE_IN_SESSION,    // Host (after MP_Host) or client (after WELCOME).
    MP_STATE_ERROR,         // Session failed or ended abnormally. See MP_GetErrorReason().
                            // Sticky until MP_Leave(), MP_Host() or MP_Join() is called.
};

// Reason codes. Values 1..31 are also REJECT reason codes on the wire
// (protocol.md section 6.3) and must not be renumbered. 32+ are local only.
enum
{
    MP_REASON_NONE              = 0,
    // --- Wire REJECT reasons (sent by the host) ---
    MP_REASON_VERSION           = 1,  // Protocol version mismatch.
    MP_REASON_BUILD             = 2,  // Build hash mismatch.
    MP_REASON_FULL              = 3,  // All MP_MAX_PLAYERS slots taken.
    MP_REASON_BAD_DATA          = 4,  // Malformed HELLO / player summary.
    // 5..31 reserved for future REJECT reasons.
    // --- Local reasons ---
    MP_REASON_TIMEOUT           = 32, // Client: no WELCOME/REJECT within MP_CONNECT_TIMEOUT_MS.
    MP_REASON_CONNECTION_LOST   = 33, // Idle timeout or transport-level disconnect.
    MP_REASON_HOST_CLOSED       = 34, // Host sent BYE(HOST_SHUTDOWN).
    MP_REASON_KICKED            = 35, // Host sent BYE(PROTOCOL_ERROR) to us.
    MP_REASON_PROTOCOL_ERROR    = 36, // We received malformed data and dropped the peer.
    MP_REASON_BIND_FAILED       = 37, // MP_Host: could not open the UDP port (in use?).
    MP_REASON_BAD_ADDRESS       = 38, // MP_Join: address is not a valid IPv4 dotted quad / port.
    MP_REASON_TRANSPORT_FAILED  = 39, // ENet failed to initialise or to create a host.
    MP_REASON_LEFT              = 40, // The player left normally (BYE LEAVING). Not an error.
};

// ---------------------------------------------------------------------------
// Player summary (who a player is). Built from the local save by PS-013.
// Wire layout: protocol.md section 5. In-memory layout is NOT the wire layout.
// ---------------------------------------------------------------------------

enum
{
    MP_AVATAR_STYLE_PLAYER,     // Brendan / May by gender (default, M1).
    MP_AVATAR_STYLE_COUNT,
};

struct MpPlayerSummary
{
    u8 name[MP_PLAYER_NAME_SIZE]; // Game charset. Always contains EOS; bytes after it are EOS.
    u8 gender;                    // MALE or FEMALE.
    u8 avatarStyle;               // MP_AVATAR_STYLE_*.
    u8 badges;                    // Bitfield: bit i set = FLAG_BADGE01_GET + i obtained (i = 0..7).
                                  // UI derives the count (popcount); there is no count field.
    u8 summaryFlags;              // Reserved, always 0 in M1.
    u32 trainerId;                // Full 32-bit ID: low 16 bits = public TID, high 16 = secret ID.
};

// ---------------------------------------------------------------------------
// Presence (where a player is and how they move). PS-030 owns capture.
// Wire layout: protocol.md section 6.8.
// ---------------------------------------------------------------------------

enum
{
    MP_MOVE_NONE,           // Standing still (or idle keep-alive).
    MP_MOVE_TURN,           // Turned in place to face `facing`.
    MP_MOVE_WALK,           // One-tile step at normal speed (16 frames).
    MP_MOVE_WALK_FAST,      // One-tile step at fast speed: running, surfing, acro bike (8 frames).
    MP_MOVE_WALK_FASTER,    // One-tile step at mach bike top speed (4 frames).
    MP_MOVE_JUMP,           // Ledge jump (2 tiles in `facing`).
    MP_MOVE_JUMP_IN_PLACE,  // Acro bike hop in place.
    MP_MOVE_WARP,           // Discontinuous position change (warp, fly, teleport). Receiver snaps.
    MP_MOVE_COUNT,
};

enum
{
    MP_AVATAR_ON_FOOT,
    MP_AVATAR_MACH_BIKE,
    MP_AVATAR_ACRO_BIKE,
    MP_AVATAR_SURFING,
    MP_AVATAR_UNDERWATER,
    MP_AVATAR_STATE_COUNT,
};

enum
{
    MP_BUSY_NONE,           // Free roaming in the overworld.
    MP_BUSY_BATTLE,
    MP_BUSY_MENU,           // Start menu, bag, party, PC, etc.
    MP_BUSY_SCRIPT,         // Script / cutscene / dialogue running.
    MP_BUSY_WARPING,        // Fading between maps.
    MP_BUSY_NOT_IN_FIELD,   // Not in the overworld at all (title, main menu, ...). Map is MP_MAP_NONE.
    MP_BUSY_COUNT,
};

struct MpPresence
{
    u16 seq;            // Set by the MP layer on send; ignored in MP_SetLocalPresence.
    u8 mapGroup;        // MP_MAP_NONE when not in the overworld.
    u8 mapNum;          // MP_MAP_NONE when not in the overworld.
    s16 x;              // Map-local tile coords (object event coords minus MAP_OFFSET).
    s16 y;              // During a step this is the DESTINATION tile.
    u8 facing;          // DIR_SOUTH / DIR_NORTH / DIR_WEST / DIR_EAST.
    u8 movement;        // MP_MOVE_*.
    u8 avatarState;     // MP_AVATAR_*.
    u8 busy;            // MP_BUSY_*.
    u8 stepId;          // Incremented (mod 256) by the sender each time a new step/turn/jump/warp starts.
    u8 presenceFlags;   // Reserved, always 0 in M1.
};

// ---------------------------------------------------------------------------
// Player table entry
// ---------------------------------------------------------------------------

struct MpPlayer
{
    u8 slot;                        // 0..MP_MAX_PLAYERS-1. 0 = host.
    u8 epoch;                       // Changes every time this slot is (re)assigned. See protocol.md 7.4.
    bool8 isHost;
    bool8 isLocal;                  // TRUE for the entry describing this game instance.
    u16 pingMs;                     // Last measured round trip to this player's peer (0 = unknown / local).
    bool8 hasPresence;              // FALSE until the first PRESENCE from this player is accepted.
    u8 presenceOverflow;            // Set when the step queue overflowed; PS-031 should snap. Cleared by MP_ClearPresenceOverflow.
    struct MpPlayerSummary summary;
    struct MpPresence presence;     // Latest accepted presence (for local: the last value passed to MP_SetLocalPresence).
};

// ---------------------------------------------------------------------------
// Events (for UI: PS-022, PS-033). SINGLE CONSUMER.
// World code (PS-031) must not consume events; it polls MP_GetPlayer() instead.
// ---------------------------------------------------------------------------

enum
{
    MP_EVENT_NONE,
    MP_EVENT_SESSION_STARTED,   // We are IN_SESSION (host: after MP_Host; client: after WELCOME). slot = our slot.
    MP_EVENT_CONNECT_FAILED,    // Client: MP_Join failed (REJECT or timeout). reason = MP_REASON_*. State is ERROR.
    MP_EVENT_PLAYER_JOINED,     // A new player joined after we did. slot = their slot.
    MP_EVENT_PLAYER_LEFT,       // slot = their slot, reason = MP_REASON_LEFT / CONNECTION_LOST / KICKED / ...
                                // The slot is already empty when this event is polled.
    MP_EVENT_PLAYER_UPDATED,    // slot's summary changed (e.g. a new badge).
    MP_EVENT_DISCONNECTED,      // Client: session lost after being IN_SESSION. reason = MP_REASON_*. State is ERROR.
    MP_EVENT_COUNT,
};

#define MP_EVENT_QUEUE_SIZE 32  // Ring buffer. On overflow the OLDEST event is dropped (and logged).

struct MpEvent
{
    u8 type;                    // MP_EVENT_*.
    u8 slot;                    // MP_SLOT_NONE if not applicable.
    u8 reason;                  // MP_REASON_*, MP_REASON_NONE if not applicable.
    u8 name[MP_PLAYER_NAME_SIZE]; // Copy of the player's name at the time of the event (valid for JOINED/LEFT/UPDATED),
                                  // so notices still work after the slot has been freed. EOS-filled otherwise.
};

// ---------------------------------------------------------------------------
// Lifecycle (called from src/platform/sdl2.c only)
// ---------------------------------------------------------------------------

// Once at startup, after command-line parsing and before AgbMain(). Opens NO socket.
void MP_Init(void);
// Once per outer main-loop iteration (after ProcessEvents(), before the fixed-step
// MainLoop() loop), including while paused. Never blocks.
void MP_Update(void);
// On quit (end of main() and in SoftReset()). Idempotent. May block up to 250 ms to deliver BYE.
void MP_Shutdown(void);
// Verbose [MP] logging (--mp-debug). Errors and the session timeline are always logged.
void MP_SetDebugLogging(bool8 enabled);
// --mp-selftest (PS-011): runs the multiplayer self-tests without a window or a save
// (loopback transport, wire helpers; parser fuzzing from PS-012/PS-013). Prints
// "[MP] selftest OK" or "[MP] selftest FAILED (<n>)". Returns the exit status (0 / 1).
s32 MP_RunSelftest(void);

// ---------------------------------------------------------------------------
// Build identity and test overrides (protocol.md section 9)
// ---------------------------------------------------------------------------

// The build id string: the git short hash (8 lowercase hex digits) injected at build
// time, "nogit" if git was unavailable, or the --mp-fake-build override.
const char *MP_GetBuildId(void);
// FNV-1a 32 over (MP_PROTOCOL_VERSION as 2 LE bytes) + the build id string. Sent in HELLO.
u32 MP_GetBuildHash(void);
// --mp-fake-protocol <n>: advertise and expect protocol version `version` instead of
// MP_PROTOCOL_VERSION (HELLO, the host's version check, REJECT). Tests the VERSION reject.
void MP_SetFakeProtocolVersion(u16 version);
// --mp-fake-build <str>: use `buildId` (ASCII, truncated to 15 chars) instead of the real
// build id when computing MP_GetBuildHash(). Tests the BUILD reject.
// Both overrides must be called after MP_Init() and before MP_Host()/MP_Join().
void MP_SetFakeBuildId(const char *buildId);

// ---------------------------------------------------------------------------
// Dev harness (PS-002 gPlatformArgs -> MP). Called from sdl2.c after MP_Init().
// ---------------------------------------------------------------------------

enum
{
    MP_AUTOSTART_NONE,
    MP_AUTOSTART_HOST,      // gPlatformArgs.mpRole == PLATFORM_MP_ROLE_HOST
    MP_AUTOSTART_JOIN,      // gPlatformArgs.mpRole == PLATFORM_MP_ROLE_CLIENT
};

// Remembers what --host / --join asked for. Nothing happens until the player
// continues a saved game (MP_OnContinueSavedGame). `ip` is copied (ignored for HOST).
// `port` 0 = MP_DEFAULT_PORT.
void MP_SetAutoStart(u8 mode, const char *ip, u16 port);
// --name <text>: debug display-name override, used in the summary instead of the save's
// name (never written to the save). Accepts 1..7 chars of A-Z, a-z, 0-9 only and converts
// them to the game charset. Returns FALSE for anything else; the caller then prints an
// error and exits (status 2) before opening the window.
bool8 MP_SetNameOverride(const char *ascii);
// MP: hook at the end of CB2_ContinueSavedGame (overworld.c). If an auto-start is pending,
// MP_Update() performs MP_Host / MP_Join on its next call.
void MP_OnContinueSavedGame(void);
// MP: hook at the start of CB2_NewGame (overworld.c). If an auto-start is pending it is
// cancelled with "[MP] --host/--join needs an existing save; playing single-player" (AD-11).
void MP_OnNewGame(void);

// ---------------------------------------------------------------------------
// Session control (UI, PS-012 CLI hook)
// Preconditions for MP_Host / MP_Join: a save is loaded (gSaveBlock2Ptr valid, AD-11)
// and the state is OFFLINE or ERROR. The local player summary is built at call time.
// ---------------------------------------------------------------------------

// Opens UDP `port` (0 = MP_DEFAULT_PORT) on all IPv4 interfaces and becomes slot 0.
// On success: state IN_SESSION, queues MP_EVENT_SESSION_STARTED, returns TRUE.
// On failure: state ERROR (MP_REASON_BIND_FAILED / TRANSPORT_FAILED), returns FALSE.
bool8 MP_Host(u16 port);
// Starts connecting to `ip` (ASCII IPv4 dotted quad, NUL-terminated, e.g. "192.168.1.20";
// hostnames are not accepted because DNS would block) on `port` (0 = MP_DEFAULT_PORT).
// On success: state CONNECTING, returns TRUE; the result arrives later as
// MP_EVENT_SESSION_STARTED or MP_EVENT_CONNECT_FAILED.
// On failure: state ERROR (MP_REASON_BAD_ADDRESS / TRANSPORT_FAILED), returns FALSE.
bool8 MP_Join(const char *ip, u16 port);
// Leaves the session (or cancels a connect, or acknowledges ERROR). State becomes OFFLINE
// immediately; BYE delivery continues in the background for up to MP_LEAVE_FLUSH_MS.
// Safe to call in any state.
void MP_Leave(void);
// Rebuilds the local summary from the save and, if it changed, sends PLAYER_UPDATE
// (rate limited to 1/s; a pending change is sent when allowed). Call after a badge is obtained.
void MP_RefreshLocalSummary(void);

// ---------------------------------------------------------------------------
// Queries (cheap; safe to call every frame from anywhere on the main thread)
// ---------------------------------------------------------------------------

u8 MP_GetState(void);               // MP_STATE_*.
u8 MP_GetErrorReason(void);         // MP_REASON_* for the current ERROR state, else MP_REASON_NONE.
bool8 MP_IsActive(void);            // TRUE only in MP_STATE_IN_SESSION.
bool8 MP_IsHost(void);              // TRUE if IN_SESSION as the host.
u8 MP_GetLocalSlot(void);           // Our slot, or MP_SLOT_NONE if not IN_SESSION.
u8 MP_GetPlayerCount(void);         // Occupied slots including us; 0 if not IN_SESSION.
// Returns the entry for an occupied slot, or NULL if `slot` is out of range or empty,
// or if not IN_SESSION. The pointer stays valid until the next MP_Update() call.
// Iterate with: for (i = 0; i < MP_MAX_PLAYERS; i++) if ((p = MP_GetPlayer(i)) != NULL) ...
const struct MpPlayer *MP_GetPlayer(u8 slot);
u16 MP_GetPort(void);               // Port being hosted on / connected to, 0 if OFFLINE.
// ASCII name of a reason code, for logs only (UI owns its own game-charset strings).
const char *MP_GetReasonName(u8 reason);

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

// Pops the oldest event into *out. Returns FALSE (and sets out->type = MP_EVENT_NONE) if empty.
bool8 MP_PollEvent(struct MpEvent *out);

// ---------------------------------------------------------------------------
// Presence (PS-030 capture hook / PS-031 avatars)
// ---------------------------------------------------------------------------

#define MP_PRESENCE_QUEUE_SIZE   8       // Per-slot FIFO of accepted remote presence updates.

// MP: hook, once per game tick at the end of MainLoop()'s callbacks (src/main.c).
// Reads (never writes) the local player state: gSaveBlock1Ptr->location, the player
// object event, gPlayerAvatar.flags, gMain.callback2 / inBattle, the start menu task
// and the field-controls lock, and passes the result to MP_SetLocalPresence().
// Implemented in src/mp/mp_presence_capture.c. Cheap; does nothing to the game (AD-1).
void MP_CaptureLocalPresence(void);
// Called by the capture hook, typically once per game tick. Cheap: it only stores the
// value (always, so the current presence can be sent the moment a session starts).
// Out-of-range fields are sanitised (invalid map -> MP_MAP_NONE / NOT_IN_FIELD, coords
// clamped to -16..1023, bad facing -> DIR_SOUTH, bad enums -> 0), so what is sent always
// passes the receiver's checks. MP_Update() sends it while IN_SESSION when it differs from
// the last sent value (at most 20 Hz) or as a 1 s keep-alive. A stepId change always
// forces a send as soon as the 50 ms window allows; several steps inside one window merge
// into the newest state, and receivers see the stepId gap.
void MP_SetLocalPresence(const struct MpPresence *presence);
// Pops the next accepted presence update for a remote slot, in arrival order
// (per-slot FIFO of 8; on overflow the oldest is dropped and presenceOverflow is set).
// Returns FALSE if none is queued, the slot is empty or local, or not IN_SESSION.
bool8 MP_PopRemotePresence(u8 slot, struct MpPresence *out);
// Clears MpPlayer.presenceOverflow for `slot` after the consumer has snapped.
void MP_ClearPresenceOverflow(u8 slot);
// TRUE if (mapGroup, mapNum) names a map of this build: mapGroup < MAP_GROUPS_COUNT and
// mapNum < the number of maps in that group (a constant table built from the last map id of
// each group in the generated constants/map_groups.h, checked at compile time against
// MAP_GROUPS_COUNT). Pure. Never index gMapGroups or call
// Get_MapHeader() with values from the network without this check.
bool8 MP_IsValidMap(u8 mapGroup, u8 mapNum);

// ---------------------------------------------------------------------------
// Remote avatars (PS-031): other players drawn as object events on your map
// ---------------------------------------------------------------------------

// Object event localIds reserved for remote avatars: MP_AVATAR_LOCAL_ID_BASE + slot
// (0xE0..0xE3). No map object uses them (the special ids are 127 camera, 236-240 Berry
// Blender, 255 player). Avatars have no object template, flagId 0 and are never saved.
#define MP_AVATAR_LOCAL_ID_BASE  0xE0

// TRUE if `localId` is a remote avatar's (MP_AVATAR_LOCAL_ID_BASE .. + MP_MAX_PLAYERS - 1).
// Used by the "// MP:" hooks that make avatars non-solid, non-interactive and unsaved.
bool8 MP_IsRemoteAvatarLocalId(u8 localId);
// MP: hook, once per game tick at the end of MainLoop() (after MP_CaptureLocalPresence).
// Only acts while the overworld callback runs: spawns / despawns / animates the avatars of
// remote players on the current map from MP_GetPlayer() and MP_PopRemotePresence().
// Implemented in src/mp/mp_avatars.c. Does nothing in single-player beyond a state check.
void MP_UpdateRemoteAvatars(void);
// MP: hook at the start of SaveObjectEvents() (load_save.c), i.e. before every copy of
// gObjectEvents into the save. Removes all remote avatars (they are re-created on the next
// field tick). Safe in any callback: outside the overworld only the object event is
// deactivated, because the sprite system then belongs to another screen.
void MP_RemoveRemoteAvatars(void);
// MP: hook in InitObjectEventStateFromTemplate (event_object_movement.c, PS-032): every object
// slot is taken and the game needs one. Removes the remote avatar of the highest slot (it may
// come back later if the budget allows). Returns TRUE if a slot was freed.
bool8 MP_EvictRemoteAvatarForGame(void);
// MP: hook in RemoveObjectEvent (event_object_movement.c, PS-032), for avatar localIds only,
// before the object's sprite is destroyed: drops the avatar's surf blob and own palette.
void MP_OnObjectEventRemoved(u8 objectEventId);

#endif // GUARD_MP_MP_H
