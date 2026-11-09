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

#define MP_PROTOCOL_VERSION      9       // Wire protocol version. Bump on ANY wire change. Max 255. 2 = INTERACT_* (PS-041), 3 = LINK_* (PS-046), 4 = trainer card (PS-042), 5 = view party (PS-043), 6 = TRADE_COMMIT (PS-044), 7 = trade journal recovery (PS-050), 8 = INTERACT kind BATTLE_DOUBLE (PS-045), 9 = LINK_DONE + END reason DONE (PS-051).
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
    // PS-041 player interaction (see "Interaction" below). `kind` = MP_INTERACT_*.
    MP_EVENT_INTERACT_REQUEST,  // Someone asks us to trade/battle. slot = requester. Answer with MP_RespondInteract().
    MP_EVENT_INTERACT_RESULT,   // Our request ended. slot = target, result = MP_INTERACT_RESULT_*.
    MP_EVENT_INTERACT_CANCELLED,// The request to us is gone (requester cancelled or left, or it timed out).
                                // slot = requester, result = MP_INTERACT_RESULT_CANCELLED / TIMEOUT.
    MP_EVENT_COUNT,
};

#define MP_EVENT_QUEUE_SIZE 32  // Ring buffer. On overflow the OLDEST event is dropped (and logged).

struct MpEvent
{
    u8 type;                    // MP_EVENT_*.
    u8 slot;                    // MP_SLOT_NONE if not applicable.
    u8 reason;                  // MP_REASON_*, MP_REASON_NONE if not applicable.
    u8 kind;                    // MP_INTERACT_* for the INTERACT events, else 0.
    u8 result;                  // MP_INTERACT_RESULT_* for the INTERACT events, else 0.
    u8 name[MP_PLAYER_NAME_SIZE]; // Copy of the player's name at the time of the event (valid for JOINED/LEFT/UPDATED
                                  // and the INTERACT events), so notices still work after the slot has been freed.
                                  // EOS-filled otherwise.
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

// PS-041: the remote player whose avatar stands on tile (x, y), or MP_SLOT_NONE. (x, y) are
// object-event coordinates (ObjectEvent.currentCoords, MAP_OFFSET included), exactly what
// GetObjectEventIdByPosition takes and what the field input code computes for the tile in
// front of the player. Only spawned, valid avatars count; only while the overworld runs.
// This is the ONE place where avatars are "found" by position: GetObjectEventIdByPosition
// still ignores them, so normal NPC interaction can never pick an avatar.
u8 MP_GetRemoteAvatarSlotAt(s16 x, s16 y);

// ---------------------------------------------------------------------------
// Interaction (PS-041): TRADE / BATTLE requests through the host (protocol.md 6.11)
// ---------------------------------------------------------------------------
// One request per requester at a time. The host checks the target (in the table, on the
// requester's map, not already asked by someone else), forwards it and enforces the
// timeout. The target answers with MP_RespondInteract(); the requester gets
// MP_EVENT_INTERACT_RESULT. Nothing here starts a trade or battle (PS-044 / PS-045).
//
// PS-042: kind TRAINER_CARD asks for the target's trainer card. There is no prompt: the
// target's session answers by itself at once (TRAINER_CARD_DATA, then ACCEPTED), and no
// MP_EVENT_INTERACT_REQUEST is queued on the target. After MP_EVENT_INTERACT_RESULT with
// ACCEPTED the validated card is available from MP_GetViewedTrainerCard().
//
// PS-043: kind VIEW_PARTY asks to see the target's party. The target is prompted like for a
// trade ("NAME wants to see your POKéMON. Allow?"). When it answers ACCEPTED its session
// sends the party (PARTY_DATA) before the RESPONSE; after MP_EVENT_INTERACT_RESULT ACCEPTED
// the validated party is available from MP_GetViewedParty().

enum
{
    MP_INTERACT_NONE,
    MP_INTERACT_TRADE,
    MP_INTERACT_BATTLE,         // PS-045: a SINGLE link battle
    MP_INTERACT_TRAINER_CARD,   // PS-042: public info, answered without a prompt
    MP_INTERACT_VIEW_PARTY,     // PS-043: needs the target's consent
    MP_INTERACT_BATTLE_DOUBLE,  // PS-045 (protocol 8): a DOUBLE link battle, prompted like BATTLE
    MP_INTERACT_KIND_COUNT,
};

enum
{
    MP_INTERACT_RESULT_NONE,
    MP_INTERACT_RESULT_ACCEPTED,
    MP_INTERACT_RESULT_DECLINED,
    MP_INTERACT_RESULT_BUSY,        // target was in a battle / menu / script (or already asked)
    MP_INTERACT_RESULT_TIMEOUT,     // no answer within MP_INTERACT_TIMEOUT_MS
    MP_INTERACT_RESULT_UNAVAILABLE, // target gone, not on your map, stale, or you already have a request out
    MP_INTERACT_RESULT_CANCELLED,   // requester cancelled (only in MP_EVENT_INTERACT_CANCELLED)
    MP_INTERACT_RESULT_BAD_DATA,    // local only (PS-042): accepted, but the data that came with it
                                    // was missing or failed validation; nothing is shown
    MP_INTERACT_RESULT_COUNT,
};

#define MP_INTERACT_TIMEOUT_MS      15000   // the target must answer within this
#define MP_INTERACT_HOST_GRACE_MS   2000    // host: extra time before it times a request out itself
#define MP_INTERACT_CLIENT_GRACE_MS 5000    // requester: extra time before it gives up locally
#define MP_INTERACT_MIN_GAP_MS      1000    // host: minimum time between two requests from one player

// Requester. FALSE (nothing sent) if not IN_SESSION, `targetSlot` is empty or ourselves,
// `kind` is invalid, or we already have a request out.
bool8 MP_SendInteractRequest(u8 targetSlot, u8 kind);
// Requester: withdraw our request (no MP_EVENT_INTERACT_RESULT follows). Safe in any state.
void MP_CancelInteractRequest(void);
// TRUE while our own request is out (between MP_SendInteractRequest and its result).
bool8 MP_IsInteractRequestPending(void);
// Target: answer the request to us with ACCEPTED, DECLINED or BUSY. Ignored if there is
// none or `result` is anything else.
void MP_RespondInteract(u8 result);
// Target: TRUE while a request to us is waiting for MP_RespondInteract().
bool8 MP_HasIncomingInteract(void);

// ---------------------------------------------------------------------------
// Viewing another player's data (PS-042 trainer card, protocol.md 6.13)
// ---------------------------------------------------------------------------
// Remote data lives in session memory only and is NEVER written into the save (AD-10).

#define MP_TRAINER_CARD_PROFILE_WORDS 4 // = TRAINER_CARD_PROFILE_LENGTH (checked in mp_playerdata.c)

// A trainer card as it travels on the wire, already validated and clamped (protocol.md 6.12).
struct MpTrainerCard
{
    u8 gender;                      // MALE / FEMALE
    u8 stars;                       // 0..4
    bool8 hasPokedex;
    bool8 caughtAllHoenn;
    bool8 hasAllPaintings;
    bool8 hasAllFrontierSymbols;
    u16 hofDebutHours;              // <= 999
    u8 hofDebutMinutes;             // <= 59
    u8 hofDebutSeconds;             // <= 59
    u16 caughtMonsCount;            // <= 999
    u16 trainerId;                  // public 16-bit ID
    u16 playTimeHours;              // <= 999
    u8 playTimeMinutes;             // <= 59
    u16 linkBattleWins;             // <= 9999
    u16 linkBattleLosses;           // <= 9999
    u16 battleTowerWins;            // <= 9999
    u16 battleTowerStraightWins;    // <= 9999
    u16 contestsWithFriends;        // <= 999
    u16 pokeblocksWithFriends;
    u16 pokemonTrades;
    u16 frontierBP;                 // <= 9999
    u32 money;                      // <= 999999
    u16 easyChatProfile[MP_TRAINER_CARD_PROFILE_WORDS]; // EC_EMPTY_WORD or a word in a valid group
    u8 name[MP_PLAYER_NAME_SIZE];   // protocol.md 5.1.1 rules, EOS padded
};

// The local player's card, built from the loaded save (read only; src/mp/mp_playerdata.c).
// Uses the --name override like the player summary. Always passes the receiver's checks.
void MP_BuildLocalTrainerCard(struct MpTrainerCard *out);
// Requester: the card that came with the last ACCEPTED TRAINER_CARD request to `slot`.
// FALSE if there is none (another slot, a newer request, the session ended).
bool8 MP_GetViewedTrainerCard(u8 slot, struct MpTrainerCard *out);
// Forget any received view data (call after showing it). Safe in any state.
void MP_ClearViewedData(void);

// PS-043 (protocol.md 6.14). A Pokemon travels as the game's own 100-byte encrypted and
// checksummed struct Pokemon (the link trade format; its layout has no padding or pointers,
// so it is the same on 32- and 64-bit builds; static-asserted in mp_playerdata.c).
#define MP_MON_WIRE_SIZE 100

// The local party as raw mons (read only: copies of gPlayerParty, never written back). Only
// mons that pass MP_ValidateMon are included, in party order. Returns the count (0..6).
u8 MP_BuildLocalParty(u8 mons[PARTY_SIZE][MP_MON_WIRE_SIZE]);
// TRUE if `raw` (MP_MON_WIRE_SIZE bytes) is a mon the summary screen can show safely: the
// checksum matches after decryption, not a bad egg, species 1..NUM_SPECIES-1 and not an
// unused "old Unown" slot, level 1..MAX_LEVEL, moves < MOVES_COUNT, held item < ITEMS_COUNT,
// hp <= maxHP, markings < 16, language <= LANGUAGE_SPANISH, no text control codes in the
// nickname or OT name. Works on a local copy; `raw` is never modified. Pure.
bool8 MP_ValidateMon(const u8 *raw);
// Requester: the party that came with the last ACCEPTED VIEW_PARTY request to `slot`. Copies
// *countOut mons (0..6, each MP_MON_WIRE_SIZE raw bytes, all validated) into `monsOut`.
// FALSE if there is none.
bool8 MP_GetViewedParty(u8 slot, u8 *countOut, u8 monsOut[PARTY_SIZE][MP_MON_WIRE_SIZE]);

// ---------------------------------------------------------------------------
// Virtual link cable (PS-046, protocol.md 6.12, design.md "As implemented in PS-046")
// ---------------------------------------------------------------------------
//
// Emerald's own cable stack (link.c: OpenLink, SendBlock, the player-data exchange,
// standby, close-link, trade, link battle) runs over the session. The host sequences
// "rows" of one 8-word link command per member and sends every row to all members,
// the sender included, exactly as the SIO multi-mode cable does.
//
// Usage (PS-041 / PS-044 / PS-045): once the players have agreed, every member calls
// MP_LinkJoin() with the same slot mask. When MP_LinkGetState() == MP_LINK_STATE_GROUPED on
// this instance, the normal game code can call OpenLink() and everything above it. Link ids
// (GetMultiplayerId()) follow slot order: the lowest slot in the mask is link id 0, the
// link master. If the group ends while the cable is open (a member left the link or the
// session, a queue overflowed, the watchdog fired) the game's own link-error path runs
// (LINK_STAT_ERROR_HARDWARE -> CB2_LinkError). Without a group, the cable behaves like an
// unplugged one: nothing is ever received, as in single-player.

enum
{
    MP_LINK_STATE_NONE,         // Not in a link group.
    MP_LINK_STATE_REQUESTED,    // MP_LinkJoin sent; waiting for the other members.
    MP_LINK_STATE_GROUPED,      // In a link group: the cable is plugged in.
};

enum
{
    MP_LINK_END_NONE            = 0,
    // --- Wire reasons (LINK_END from the host) ---
    MP_LINK_END_LEFT            = 1,  // A member called MP_LinkLeave (or joined another group).
    MP_LINK_END_LOST            = 2,  // A member left the session.
    MP_LINK_END_OVERFLOW        = 3,  // A member sent link commands faster than the bus can carry.
    MP_LINK_END_REFUSED         = 4,  // MP_LinkJoin: bad mask, or a slot in it is empty.
    MP_LINK_END_DONE            = 5,  // PS-051: every member sent LINK_DONE (or one did, plus the grace time). A normal end.
    // 6..31 reserved for future wire reasons.
    // --- Local reasons ---
    MP_LINK_END_SESSION         = 32, // Our own session ended (left, lost, kicked).
    MP_LINK_END_WATCHDOG        = 33, // Our link commands were not echoed for MP_LINK_WATCHDOG_MS.
    MP_LINK_END_RX_OVERFLOW     = 34, // More than MP_LINK_RX_QUEUE_SIZE rows waiting (game stalled).
};

#define MP_LINK_WATCHDOG_MS     10000
#define MP_LINK_DONE_GRACE_MS   3000    // PS-051: host, after the first LINK_DONE of a group
#define MP_LINK_RX_QUEUE_SIZE   256     // Rows buffered per member (about 4 s at 60 rows/s).

// Asks the host to form a link group of the slots in `slotMask` (bit i = slot i). The mask
// must contain our own slot and 2..4 occupied slots; every member must call it with the same
// mask. Leaves any previous group first. FALSE if not IN_SESSION or the mask is invalid.
bool8 MP_LinkJoin(u8 slotMask);
// Leaves the link group (or cancels a request). The other members get MP_LINK_END_LEFT.
void MP_LinkLeave(void);
u8 MP_LinkGetState(void);       // MP_LINK_STATE_*.
u8 MP_LinkGetLinkId(void);      // 0..members-1 while GROUPED, else MP_SLOT_NONE.
u8 MP_LinkGetEndReason(void);   // Why the last group or request ended (MP_LINK_END_*).
// PS-048: TRUE while in or requesting a link group. sdl2.c then keeps pause and speed-up off,
// because the peers count link timeouts in frames.
bool8 MP_LinkTimeControlsLocked(void);

// MP: called by HandleLinkConnection() (link.c, PC build) once per game tick in place of the
// SIO LinkMain1(). Same contract: fills recvCmds[0..3] with one row (or zeros), queues
// sendCmd (and clears it), advances gLink.state and returns the gLinkStatus word.
// recvCmds has CMD_LENGTH (8) words per player (checked at compile time).
u32 MP_LinkMain1(u8 *shouldAdvanceLinkState, u16 *sendCmd, u16 (*recvCmds)[8]);
// PS-051: the agreed end of a direct link activity (mp_link_cable.c). Vanilla's close-link
// handshake assumes GBA lockstep; over the host-sequenced cable each side ends on its own:
//   MP_LinkActivityAgreed: both players agreed to stop. Link errors are suppressed and the cable
//     no longer reports the bus / group ending as a hardware error (nothing left to recover).
//   MP_LinkActivityTryFinish: once our last command is out (no link callback, gSendCmd empty;
//     at most ~2 s), LINK_DONE to the host and CloseLink(). TRUE when closed. The host ends the
//     group (END(DONE)) when every member is done, or MP_LINK_DONE_GRACE_MS after the first.
//   MP_LinkIsFinishing: we agreed, or a partner is done, or the group ended DONE.
void MP_LinkActivityAgreed(const char *who);
bool8 MP_LinkActivityIsAgreed(void);
bool8 MP_LinkActivityTryFinish(void);
bool8 MP_LinkIsFinishing(void);
// PS-051: link flight recorder. MP_LinkTraceFrame once per tick (HandleLinkConnection) keeps the
// last 600 ticks while a link is in use; MP_LinkTraceDump writes them to the log file with a
// "[MP] ==== link trace (reason) ====" header (one dump per incident: triggers within 2 s are
// one incident). Triggers: CB2_LinkError, MP_LinkRejectData, a cable hardware error, and a link
// error seen by TrySetLinkErrorBuffer (suppressed or not).
void MP_LinkTraceFrame(void);
void MP_LinkTraceDump(const char *reason);
u32 MP_LinkTraceGetDumpCount(void); // since boot (selftest)
// --mp-link-test (dev harness): slots 0 and 1 link up, run the player-data exchange and
// SendBlock rounds, close, and repeat, logging each step. Ticked from HandleLinkConnection().
void MP_SetLinkTest(bool8 enabled);
void MP_LinkTestTick(void);

// ---------------------------------------------------------------------------
// Direct trade over the cable (PS-044, protocol.md 6.15, src/mp/mp_trade.c + mp_trade_commit.c)
// ---------------------------------------------------------------------------
//
// After PS-041's TRADE request is accepted, both players save, link up over the virtual
// cable and enter Emerald's own trade menu (CB2_StartCreateTradeMenu) from the overworld.
//
// Host-arbitrated commit (Director ruling on PS-044). The trade's save is two-phase
// (CB2_SaveAndEndTrade: write every sector, write the last one unsigned, then sign it). For a
// direct trade the vanilla standby between "unsigned" and "signed" is replaced by:
//   1. MP_TradeCommitReady(): "my save is written; only the signature is left" (READY to the host).
//   2. The host answers COMMIT to every member once BOTH members of the link group are READY,
//      or ABORT if the group ends, a member leaves, or the round is not complete within
//      MP_TRADE_COMMIT_TIMEOUT_MS. The host's decision is final and repeated on request.
//   3. MP_TradeCommitPoll(): COMMIT -> sign the save; ABORT -> do not sign (the old save stays
//      valid and the link-error path reloads it).
// A member whose own session ends while it waits gets ABORT (IN_DOUBT): see protocol.md 6.15
// for the one remaining window.
enum
{
    MP_TRADE_COMMIT_IDLE,       // MP_TradeCommitReady not called (or MP_TradeCommitReset since)
    MP_TRADE_COMMIT_WAITING,    // READY sent, no decision yet: keep waiting
    MP_TRADE_COMMIT_COMMIT,     // sign the save
    MP_TRADE_COMMIT_ABORT,      // do not sign; MP_TradeCommitGetAbortReason() says why
};
enum
{
    MP_TRADE_ABORT_NONE         = 0,
    // --- Wire reasons (TRADE_COMMIT ABORT from the host) ---
    MP_TRADE_ABORT_GROUP_ENDED  = 1,  // the link group ended (a member left the link or the session)
    MP_TRADE_ABORT_TIMEOUT      = 2,  // the other member was not ready within MP_TRADE_COMMIT_TIMEOUT_MS
    MP_TRADE_ABORT_MISMATCH     = 3,  // not in a 2-member group with that id, or the members disagree on seq
    MP_TRADE_ABORT_FIRST        = 1,
    MP_TRADE_ABORT_LAST         = 3,
    // --- Local reasons ---
    MP_TRADE_ABORT_NOT_GROUPED  = 32, // MP_TradeCommitReady without a link group
    MP_TRADE_ABORT_IN_DOUBT     = 33, // our session ended before the decision arrived (presumed abort)
};
#define MP_TRADE_COMMIT_TIMEOUT_MS  15000
// Member: sends READY for the next trade of the current link group. FALSE (and the state is
// ABORT, NOT_GROUPED) if we are not GROUPED.
bool8 MP_TradeCommitReady(u32 tradeId); // PS-050: tradeId = the journal's (MpTradeJournal_ComputeId), != 0
u8 MP_TradeCommitPoll(void);            // MP_TRADE_COMMIT_*
u8 MP_TradeCommitGetAbortReason(void);  // MP_TRADE_ABORT_* for the last ABORT
void MP_TradeCommitReset(void);         // back to IDLE (the round counter is kept)

// Game side (src/mp/mp_trade.c). TRUE while a trade started by PS-044 runs (from the linkup
// until the player is back in the overworld). trade.c uses it, marked "// MP:", to send its
// exits to the normal overworld and to use the commit above in CB2_SaveAndEndTrade.
bool8 MP_TradeIsDirect(void);
// The trade menu's exit for a direct trade (instead of CB2_ReturnToFieldFromMultiplayer, which
// re-enters the cable-room overworld): leaves the link group and returns to the normal field.
void CB2_MpReturnToFieldAfterTrade(void);
// MP: from MP_LinkPrepareErrorRecovery (link.c): a link error ended the trade; forget it.
void MP_TradeOnLinkErrorRecovery(void);
// MP: PS-051 (replaces BUG-005's MP_TradeOnMenuExit). From trade.c at the moment both players
// agreed to stop trading: the leader when it sends LINKCMD_BOTH_CANCEL_TRADE, the follower when
// it receives it. From here on the exit never depends on the partner: no link error screen,
// the bus or group ending is the normal end (MP_LinkActivityAgreed).
void MP_TradeOnBothCancel(const char *who);
// MP: PS-051, from trade.c's CB_InitExitCanceledTrade (direct trade, after the fade): closes our
// own cable locally once our last link command is out (`queueEmpty`: the trade menu's queued
// sends are done). No vanilla close handshake. TRUE when the cable is closed: go on to
// CB_ExitCanceledTrade, which frees the menu and returns to the field.
bool8 MP_TradeTryExitLocally(bool8 queueEmpty);
// MP: PS-051, from CB2_LinkError (link.c). TRUE if a direct trade was already exiting by
// agreement: then the link error is turned into the normal return to the field (logged with a
// trace dump) and CB2_LinkError must return at once.
bool8 MP_TradeLinkErrorDuringAgreedExit(void);
// trade.c (PS-051): the trade menu's callbackId for the flight recorder, 0xFF if no menu is on
// screen; and freeing the menu for the safety net above (nothing if it is not allocated).
u8 MP_TradeMenuTraceId(void);
void MP_TradeFreeMenuForExit(void);
// BUG-007: a log line from trade.c ("trade: <what>"); the same text twice in a row is logged once.
void MP_TradeLog(const char *what);

// ---------------------------------------------------------------------------
// Direct link battle over the cable (PS-045, src/mp/mp_battle.c, pure parts in mp_battle.h)
// ---------------------------------------------------------------------------
// After a BATTLE / BATTLE_DOUBLE request is accepted, both players save, link up and start
// Emerald's own 2-player cable link battle from the overworld (as Task_StartWiredCableClubBattle,
// without the colosseum). The decomp hooks ("// MP: PS-045") use MP_BattleIsDirect() to
//   - send CB2_ReturnFromCableClubBattle's exit to the normal field (CB2_MpReturnToFieldAfterBattle),
//   - save the post-battle link records at the player's position (no Cable Club dynamic warp),
//   - skip the Frontier "record this battle?" step (a second linkup and a save change).
// TRUE from the start of the battle until the player is back in the overworld.
bool8 MP_BattleIsDirect(void);
void CB2_MpReturnToFieldAfterBattle(void);
// MP: from CB2_ReturnFromCableClubBattle (cable_club.c): the battle is over; only the local
// post-battle save and the field return are left (the battle watchdog stops).
void MP_BattleOnBattleEnd(void);
// MP: PS-051, from SetBattleEndCallbacks (battle_controller_player.c) for a direct battle: the
// engine sent ENDLINKBATTLE, the battle is over for both. Instead of the vanilla close handshake,
// agree and close our own cable once our last command is out (MP_LinkActivityTryFinish).
// TRUE when the cable is closed.
bool8 MP_BattleTryExitLocally(void);
// MP: from MP_LinkPrepareErrorRecovery (link.c): a link error ended the battle (or its start).
void MP_BattleOnLinkErrorRecovery(void);

// ---------------------------------------------------------------------------
// Trade journal (PS-050, protocol.md 6.16, design.md "Trade journal")
// ---------------------------------------------------------------------------
//
// "<save>.trade" (crash-safe) records a direct trade from just before READY until both sides
// are known to have the same outcome. Its state: READY (written before READY is sent; the save
// on disk is the pre-trade one) or COMMIT (written when COMMIT is known, BEFORE the save is
// signed; the trade is then applied to this save, now or at the next load). Unsure sides
// exchange TRADE_RECOVER with the partner (and the host, which remembers its decisions) and
// roll forward or delete the journal so both saves agree. A pending journal blocks new trades.
enum
{
    MP_TJ_STATUS_NONE,
    MP_TJ_STATUS_READY,         // my journal for it is READY: I have not signed (unsure)
    MP_TJ_STATUS_COMMITTED,     // the trade is COMMIT on my side (journal COMMIT)
    MP_TJ_STATUS_NONE_HAS,      // no journal for it, and I have the mon you gave (it happened)
    MP_TJ_STATUS_NONE_HASNOT,   // no journal for it, and I do not have it (it did not happen)
    MP_TJ_STATUS_BUSY,          // that trade is still running here: ask again
    MP_TJ_STATUS_DAMAGED,       // my journal is unreadable: I cannot say
    MP_TJ_STATUS_ABORTED,       // host only: its decision for that trade was ABORT
    MP_TJ_STATUS_COUNT,
};
// Session side (mp_trade_commit.c): send one TRADE_RECOVER (routed through the host; to the host's
// own game directly), and pop received ones in arrival order (queue of MP_TRADE_RECOVER_QUEUE).
#define MP_TRADE_RECOVER_QUEUE 8
struct MpMsgTradeRecover;
void MP_TradeRecoverSend(const struct MpMsgTradeRecover *msg);
bool8 MP_TradeRecoverPoll(struct MpMsgTradeRecover *out);
// Game side (src/mp/mp_trade.c).
void MP_TradeJournalOnLoad(void);       // MP: end of CB2_ContinueSavedGame: read and check the journal
bool8 MP_TradeJournalIsPending(void);   // a journal is on disk (blocks new trades)
// trade.c (// MP: PS-050): before TradeMons (the two mons and our slot), before READY (the
// received mon after any evolution; FALSE = not written, do not send READY), on COMMIT (before
// signing; FALSE = not written, do not sign), once signed, and on a final ABORT (not IN_DOUBT).
struct Pokemon;
void MP_TradeJournalStage(const struct Pokemon *given, const struct Pokemon *received, u8 partyIndex, u32 partnerTid, const u8 *partnerName);
bool8 MP_TradeJournalWriteReady(u32 *tradeIdOut);    // the received mon is read from the staged party slot
void MP_TradeJournalOnSigned(void);                 // the save with the trade is signed and stored
bool8 MP_TradeJournalMarkCommit(void);
void MP_TradeJournalOnAbort(void);

// ---------------------------------------------------------------------------
// Link receive-path hardening (PS-047, AD-9). src/mp/mp_link_validate.c
// ---------------------------------------------------------------------------
//
// The decomp's link code trusts the peer completely. These helpers are called, marked
// "// MP:", where link.c / battle_*.c / trade.c / trainer_card.c consume data that came from
// another game: they clamp or reject anything that would index a table, size a copy or
// overflow a buffer. They work on the data itself, so they protect whatever transport
// delivered it (virtual cable, a PS-042/043 message, a future one).

struct Pokemon;
struct BattlePokemon;
struct LinkPlayer;
struct TrainerCard;
struct Mail;

// A peer sent data the game must not use. Logs `what`, counts it, and if a link cable is
// open makes it report LINK_STAT_ERROR_HARDWARE, so the game's own link-error path ends the
// activity (CB2_LinkError, recovery is PS-048). The caller still drops or neutralises the data.
void MP_LinkRejectData(const char *what);
// BUG-006: the same with a printf-style reason (the check, the field and the value).
void MP_LinkRejectDataF(const char *fmt, ...);
u32 MP_LinkGetRejectCount(void);    // since boot (selftest / --mp-debug)

// A received party Pokémon. Empty slots (species NONE) are zeroed and accepted. Anything
// that fails the checksum, or has a species / held item / move / level out of range, is
// zeroed and FALSE is returned. Harmless out-of-range fields (language, mail index, hp >
// max hp, contest ribbon counts) are clamped in place. `mon` keeps its encryption.
bool8 MP_LinkValidateMon(struct Pokemon *mon);
// MP_LinkValidateMon on `count` mons; FALSE if any was rejected (all are still processed).
bool8 MP_ValidateParty(struct Pokemon *party, u32 count);
// BUG-006/007: the reason of the last refusal by MP_LinkValidateMon / MP_ValidateParty, for the log.
const char *MP_LinkValidateReason(void);
// A struct BattlePokemon from the peer (switch-in data, GetMonData replies, SETMONDATA):
// species, moves and held item out of range are cleared and give FALSE (no honest game sends
// them). Ability, types, level, stat stages, hp and the strings are clamped silently: honest
// games leave some of them unset in transit.
bool8 MP_SanitizeBattleMon(struct BattlePokemon *mon);
// A received LinkPlayer, before ConvertLinkPlayerName(): EOS-terminated name (5 chars for
// LANGUAGE_JAPANESE, which ConvertInternationalString grows by 4 bytes), language 1..7,
// gender 0..1, battler id 0..3. FALSE if anything changed.
bool8 MP_SanitizeLinkPlayer(struct LinkPlayer *player);
// A trainer card about to be shown (ShowTrainerCardInLink): stars 0..4, gender 0..1, name
// terminated, FRLG-only icon species and stickers in range. FALSE if anything changed.
bool8 MP_SanitizeTrainerCard(struct TrainerCard *card);
// A received struct Mail: not a mail item -> cleared (FALSE); species out of range -> NONE
// (FALSE); the name is terminated silently.
bool8 MP_SanitizeMail(struct Mail *mail);
// A language byte from the peer: 1..NUM_LANGUAGES, else LANGUAGE_ENGLISH.
u8 MP_SanitizeLanguage(u8 language);

#endif // GUARD_MP_MP_H
