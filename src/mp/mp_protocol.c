// Pure message parsers and writers (protocol.md sections 3-6, 8.3).
// Nothing here touches global state, does I/O or allocates.

#include <string.h>
#include "gba/types.h"
#include "gba/defines.h"
#include "mp/mp.h"
#include "mp/mp_protocol.h"
#include "mp/mp_summary.h"
#include "mp/mp_wire.h"

static const u8 sHelloMagic[4] = { 0x50, 0x53, 0x4D, 0x50 }; // "PSMP"

bool8 MpSeqIsNewer(u16 s, u16 last)
{
    u16 d = (u16)(s - last);
    return d != 0 && d < 0x8000;
}

// ---------------------------------------------------------------------------
// Header
// ---------------------------------------------------------------------------

bool8 MpProto_ParseHeader(const u8 *buf, u32 len, struct MpMsgHeader *out)
{
    struct MpReader r;

    memset(out, 0, sizeof(*out));
    if (buf == NULL || len < MP_HEADER_SIZE || len > MP_MAX_PACKET_SIZE)
        return FALSE;
    MpReader_Init(&r, buf, len);
    out->type = MpReadU8(&r);
    out->version = MpReadU8(&r);
    out->length = MpReadU16(&r);
    if (r.error || out->length != len - MP_HEADER_SIZE)
    {
        memset(out, 0, sizeof(*out));
        return FALSE;
    }
    return TRUE;
}

static void MpWriteHeader(struct MpWriter *w, u8 type, u8 version, u16 length)
{
    MpWriteU8(w, type);
    MpWriteU8(w, version);
    MpWriteU16(w, length);
}

static u16 MpFinishWrite(const struct MpWriter *w)
{
    return w->error ? 0 : (u16)w->pos;
}

// Exact-size helper: OK, TRUNCATED or OVERSIZED.
static u8 MpCheckExactSize(const u8 *payload, u32 len, u32 size)
{
    if (payload == NULL && len != 0)
        return MP_PARSE_ERR_TRUNCATED;
    if (len < size)
        return MP_PARSE_ERR_TRUNCATED;
    if (len > size)
        return MP_PARSE_ERR_OVERSIZED;
    return MP_PARSE_OK;
}

// ---------------------------------------------------------------------------
// HELLO (protocol.md 6.1)
// ---------------------------------------------------------------------------

u8 MpProto_ParseHelloPrefix(const u8 *payload, u32 len, u16 *protocolVersion, u32 *buildHash)
{
    struct MpReader r;
    u8 magic[4];

    *protocolVersion = 0;
    *buildHash = 0;
    if (payload == NULL || len < MP_HELLO_PREFIX_SIZE)
        return MP_PARSE_ERR_TRUNCATED;
    MpReader_Init(&r, payload, len);
    MpReadBytes(&r, magic, sizeof(magic));
    if (memcmp(magic, sHelloMagic, sizeof(magic)) != 0)
        return MP_PARSE_ERR_BAD_MAGIC;
    *protocolVersion = MpReadU16(&r);
    MpReadU16(&r); // reserved
    *buildHash = MpReadU32(&r);
    return r.error ? MP_PARSE_ERR_TRUNCATED : MP_PARSE_OK;
}

u8 MpProto_ParseHello(const u8 *payload, u32 len, struct MpMsgHello *out)
{
    struct MpReader r;
    u8 result;

    memset(out, 0, sizeof(*out));
    result = MpCheckExactSize(payload, len, MP_HELLO_SIZE);
    if (result != MP_PARSE_OK)
        return result;
    result = MpProto_ParseHelloPrefix(payload, len, &out->protocolVersion, &out->buildHash);
    if (result != MP_PARSE_OK)
    {
        memset(out, 0, sizeof(*out));
        return result;
    }
    MpReader_Init(&r, payload, len);
    r.pos = 12;
    out->buildFlags = MpReadU8(&r);
    if (!MP_ReadPlayerSummary(&payload[16], len - 16, &out->summary))
    {
        memset(out, 0, sizeof(*out));
        return MP_PARSE_ERR_BAD_SUMMARY;
    }
    return MP_PARSE_OK;
}

u16 MpProto_WriteHello(u8 *buf, u16 cap, const struct MpMsgHello *msg)
{
    struct MpWriter w;
    u8 summary[MP_PLAYER_SUMMARY_WIRE_SIZE];

    if (MP_WritePlayerSummary(summary, sizeof(summary), &msg->summary) == 0)
        return 0;
    MpWriter_Init(&w, buf, cap);
    MpWriteHeader(&w, MP_MSG_HELLO, (u8)(msg->protocolVersion & 0xFF), MP_HELLO_SIZE);
    MpWriteBytes(&w, sHelloMagic, sizeof(sHelloMagic));
    MpWriteU16(&w, msg->protocolVersion);
    MpWriteU16(&w, 0);
    MpWriteU32(&w, msg->buildHash);
    MpWriteU8(&w, msg->buildFlags);
    MpWriteU8(&w, 0);
    MpWriteU8(&w, 0);
    MpWriteU8(&w, 0);
    MpWriteBytes(&w, summary, sizeof(summary));
    return MpFinishWrite(&w);
}

// ---------------------------------------------------------------------------
// PlayerEntry (protocol.md 5.2)
// ---------------------------------------------------------------------------

// Parses one entry at payload[0..19]. len must be >= 20.
static u8 MpParseEntryAt(const u8 *payload, struct MpMsgPlayerEntry *out)
{
    memset(out, 0, sizeof(*out));
    out->slot = payload[0];
    out->epoch = payload[1];
    out->entryFlags = payload[2] & MP_ENTRY_FLAG_HOST; // bits 1-7 reserved, ignored
    if (out->slot >= MP_MAX_PLAYERS || out->epoch == 0)
        return MP_PARSE_ERR_BAD_FIELD;
    if (((out->entryFlags & MP_ENTRY_FLAG_HOST) != 0) != (out->slot == 0))
        return MP_PARSE_ERR_BAD_FIELD;
    if (!MP_ReadPlayerSummary(&payload[4], MP_PLAYER_SUMMARY_WIRE_SIZE, &out->summary))
        return MP_PARSE_ERR_BAD_SUMMARY;
    return MP_PARSE_OK;
}

static void MpWriteEntry(struct MpWriter *w, const struct MpMsgPlayerEntry *e)
{
    u8 summary[MP_PLAYER_SUMMARY_WIRE_SIZE];

    if (MP_WritePlayerSummary(summary, sizeof(summary), &e->summary) == 0)
    {
        w->error = TRUE;
        return;
    }
    MpWriteU8(w, e->slot);
    MpWriteU8(w, e->epoch);
    MpWriteU8(w, (e->slot == 0) ? MP_ENTRY_FLAG_HOST : 0);
    MpWriteU8(w, 0);
    MpWriteBytes(w, summary, sizeof(summary));
}

u8 MpProto_ParsePlayerEntry(const u8 *payload, u32 len, struct MpMsgPlayerEntry *out)
{
    u8 result;

    memset(out, 0, sizeof(*out));
    result = MpCheckExactSize(payload, len, MP_PLAYER_ENTRY_SIZE);
    if (result != MP_PARSE_OK)
        return result;
    result = MpParseEntryAt(payload, out);
    if (result != MP_PARSE_OK)
        memset(out, 0, sizeof(*out));
    return result;
}

u16 MpProto_WritePlayerEntry(u8 *buf, u16 cap, u8 type, const struct MpMsgPlayerEntry *msg)
{
    struct MpWriter w;

    MpWriter_Init(&w, buf, cap);
    MpWriteHeader(&w, type, MP_PROTOCOL_VERSION, MP_PLAYER_ENTRY_SIZE);
    MpWriteEntry(&w, msg);
    return MpFinishWrite(&w);
}

// ---------------------------------------------------------------------------
// WELCOME (protocol.md 6.2)
// ---------------------------------------------------------------------------

u8 MpProto_ParseWelcome(const u8 *payload, u32 len, struct MpMsgWelcome *out)
{
    u32 i, j;
    u8 n;
    u8 result;
    bool8 haveHost = FALSE;
    bool8 haveSelf = FALSE;

    memset(out, 0, sizeof(*out));
    if (payload == NULL || len < 4)
        return MP_PARSE_ERR_TRUNCATED;
    n = payload[2];
    if (n == 0 || n > MP_MAX_PLAYERS)
        return MP_PARSE_ERR_BAD_FIELD;
    if (len < MP_WELCOME_SIZE(n))
        return MP_PARSE_ERR_TRUNCATED;
    if (len > MP_WELCOME_SIZE(n))
        return MP_PARSE_ERR_OVERSIZED;

    out->yourSlot = payload[0];
    out->yourEpoch = payload[1];
    out->playerCount = n;
    if (out->yourSlot == 0 || out->yourSlot >= MP_MAX_PLAYERS || out->yourEpoch == 0)
        goto bad_field;

    for (i = 0; i < n; i++)
    {
        result = MpParseEntryAt(&payload[4 + MP_PLAYER_ENTRY_SIZE * i], &out->players[i]);
        if (result != MP_PARSE_OK)
        {
            memset(out, 0, sizeof(*out));
            return result;
        }
        for (j = 0; j < i; j++)
        {
            if (out->players[j].slot == out->players[i].slot)
                goto bad_field;
        }
        if (out->players[i].slot == 0)
            haveHost = TRUE;
        if (out->players[i].slot == out->yourSlot)
        {
            if (out->players[i].epoch != out->yourEpoch)
                goto bad_field;
            haveSelf = TRUE;
        }
    }
    if (!haveHost || !haveSelf)
        goto bad_field;
    return MP_PARSE_OK;

bad_field:
    memset(out, 0, sizeof(*out));
    return MP_PARSE_ERR_BAD_FIELD;
}

u16 MpProto_WriteWelcome(u8 *buf, u16 cap, const struct MpMsgWelcome *msg)
{
    struct MpWriter w;
    u32 i;

    if (msg->playerCount == 0 || msg->playerCount > MP_MAX_PLAYERS)
        return 0;
    MpWriter_Init(&w, buf, cap);
    MpWriteHeader(&w, MP_MSG_WELCOME, MP_PROTOCOL_VERSION, MP_WELCOME_SIZE(msg->playerCount));
    MpWriteU8(&w, msg->yourSlot);
    MpWriteU8(&w, msg->yourEpoch);
    MpWriteU8(&w, msg->playerCount);
    MpWriteU8(&w, 0);
    for (i = 0; i < msg->playerCount; i++)
        MpWriteEntry(&w, &msg->players[i]);
    return MpFinishWrite(&w);
}

// ---------------------------------------------------------------------------
// REJECT (protocol.md 6.3, frozen)
// ---------------------------------------------------------------------------

u8 MpProto_ParseReject(const u8 *payload, u32 len, struct MpMsgReject *out)
{
    struct MpReader r;

    memset(out, 0, sizeof(*out));
    if (payload == NULL || len < MP_REJECT_SIZE)
        return MP_PARSE_ERR_TRUNCATED;
    MpReader_Init(&r, payload, len);
    out->reason = MpReadU8(&r);
    MpReadU8(&r);
    out->hostProtocolVersion = MpReadU16(&r);
    out->hostBuildHash = MpReadU32(&r);
    return MP_PARSE_OK;
}

u16 MpProto_WriteReject(u8 *buf, u16 cap, const struct MpMsgReject *msg)
{
    struct MpWriter w;

    MpWriter_Init(&w, buf, cap);
    MpWriteHeader(&w, MP_MSG_REJECT, MP_PROTOCOL_VERSION, MP_REJECT_SIZE);
    MpWriteU8(&w, msg->reason);
    MpWriteU8(&w, 0);
    MpWriteU16(&w, msg->hostProtocolVersion);
    MpWriteU32(&w, msg->hostBuildHash);
    return MpFinishWrite(&w);
}

// ---------------------------------------------------------------------------
// PLAYER_LEFT, BYE, PING/PONG
// ---------------------------------------------------------------------------

u8 MpProto_ParsePlayerLeft(const u8 *payload, u32 len, struct MpMsgPlayerLeft *out)
{
    u8 result;

    memset(out, 0, sizeof(*out));
    result = MpCheckExactSize(payload, len, MP_PLAYER_LEFT_SIZE);
    if (result != MP_PARSE_OK)
        return result;
    if (payload[0] >= MP_MAX_PLAYERS || payload[1] == 0)
        return MP_PARSE_ERR_BAD_FIELD;
    out->slot = payload[0];
    out->epoch = payload[1];
    out->reason = payload[2];
    return MP_PARSE_OK;
}

u16 MpProto_WritePlayerLeft(u8 *buf, u16 cap, const struct MpMsgPlayerLeft *msg)
{
    struct MpWriter w;

    MpWriter_Init(&w, buf, cap);
    MpWriteHeader(&w, MP_MSG_PLAYER_LEFT, MP_PROTOCOL_VERSION, MP_PLAYER_LEFT_SIZE);
    MpWriteU8(&w, msg->slot);
    MpWriteU8(&w, msg->epoch);
    MpWriteU8(&w, msg->reason);
    MpWriteU8(&w, 0);
    return MpFinishWrite(&w);
}

u8 MpProto_ParseBye(const u8 *payload, u32 len, struct MpMsgBye *out)
{
    u8 result;

    memset(out, 0, sizeof(*out));
    result = MpCheckExactSize(payload, len, MP_BYE_SIZE);
    if (result != MP_PARSE_OK)
        return result;
    out->reason = payload[0];
    return MP_PARSE_OK;
}

u16 MpProto_WriteBye(u8 *buf, u16 cap, const struct MpMsgBye *msg)
{
    struct MpWriter w;

    MpWriter_Init(&w, buf, cap);
    MpWriteHeader(&w, MP_MSG_BYE, MP_PROTOCOL_VERSION, MP_BYE_SIZE);
    MpWriteU8(&w, msg->reason);
    MpWriteU8(&w, 0);
    MpWriteU8(&w, 0);
    MpWriteU8(&w, 0);
    return MpFinishWrite(&w);
}

u8 MpProto_ParsePing(const u8 *payload, u32 len, struct MpMsgPing *out)
{
    struct MpReader r;
    u8 result;

    memset(out, 0, sizeof(*out));
    result = MpCheckExactSize(payload, len, MP_PING_SIZE);
    if (result != MP_PARSE_OK)
        return result;
    MpReader_Init(&r, payload, len);
    out->timeMs = MpReadU32(&r);
    return MP_PARSE_OK;
}

u16 MpProto_WritePing(u8 *buf, u16 cap, u8 type, const struct MpMsgPing *msg)
{
    struct MpWriter w;

    MpWriter_Init(&w, buf, cap);
    MpWriteHeader(&w, type, MP_PROTOCOL_VERSION, MP_PING_SIZE);
    MpWriteU32(&w, msg->timeMs);
    return MpFinishWrite(&w);
}

// ---------------------------------------------------------------------------
// PRESENCE (protocol.md 6.8, PS-030)
// ---------------------------------------------------------------------------

u8 MpProto_ParsePresence(const u8 *payload, u32 len, struct MpMsgPresence *out)
{
    struct MpReader r;
    struct MpPresence *p = &out->presence;
    u8 result;

    memset(out, 0, sizeof(*out));
    result = MpCheckExactSize(payload, len, MP_PRESENCE_SIZE);
    if (result != MP_PARSE_OK)
        return result;
    MpReader_Init(&r, payload, len);
    out->slot = MpReadU8(&r);
    out->epoch = MpReadU8(&r);
    p->seq = MpReadU16(&r);
    p->mapGroup = MpReadU8(&r);
    p->mapNum = MpReadU8(&r);
    p->x = MpReadS16(&r);
    p->y = MpReadS16(&r);
    p->facing = MpReadU8(&r);
    p->movement = MpReadU8(&r);
    p->avatarState = MpReadU8(&r);
    p->busy = MpReadU8(&r);
    p->stepId = MpReadU8(&r);
    MpReadU8(&r); // presenceFlags: reserved, ignored
    p->presenceFlags = 0;
    if (r.error)
        goto truncated;

    if (out->slot >= MP_MAX_PLAYERS || out->epoch == 0)
        goto bad_field;
    if (p->avatarState >= MP_AVATAR_STATE_COUNT || p->busy >= MP_BUSY_COUNT)
        goto bad_field;
    if (p->mapGroup == MP_MAP_NONE || p->mapNum == MP_MAP_NONE)
    {
        // Map rule: both or neither. Not in the field: position fields are ignored.
        if (p->mapGroup != p->mapNum || p->busy != MP_BUSY_NOT_IN_FIELD)
            goto bad_field;
        p->x = 0;
        p->y = 0;
        p->facing = DIR_SOUTH;
        p->movement = MP_MOVE_NONE;
        return MP_PARSE_OK;
    }
    if (!MP_IsValidMap(p->mapGroup, p->mapNum))
        goto bad_field;
    if (p->x < MP_PRESENCE_COORD_MIN || p->x > MP_PRESENCE_COORD_MAX
     || p->y < MP_PRESENCE_COORD_MIN || p->y > MP_PRESENCE_COORD_MAX)
        goto bad_field;
    if (p->facing < DIR_SOUTH || p->facing > DIR_EAST || p->movement >= MP_MOVE_COUNT)
        goto bad_field;
    return MP_PARSE_OK;

truncated:
    memset(out, 0, sizeof(*out));
    return MP_PARSE_ERR_TRUNCATED;
bad_field:
    memset(out, 0, sizeof(*out));
    return MP_PARSE_ERR_BAD_FIELD;
}

u16 MpProto_WritePresence(u8 *buf, u16 cap, const struct MpMsgPresence *msg)
{
    struct MpWriter w;
    const struct MpPresence *p = &msg->presence;

    MpWriter_Init(&w, buf, cap);
    MpWriteHeader(&w, MP_MSG_PRESENCE, MP_PROTOCOL_VERSION, MP_PRESENCE_SIZE);
    MpWriteU8(&w, msg->slot);
    MpWriteU8(&w, msg->epoch);
    MpWriteU16(&w, p->seq);
    MpWriteU8(&w, p->mapGroup);
    MpWriteU8(&w, p->mapNum);
    MpWriteS16(&w, p->x);
    MpWriteS16(&w, p->y);
    MpWriteU8(&w, p->facing);
    MpWriteU8(&w, p->movement);
    MpWriteU8(&w, p->avatarState);
    MpWriteU8(&w, p->busy);
    MpWriteU8(&w, p->stepId);
    MpWriteU8(&w, 0); // presenceFlags: reserved
    return MpFinishWrite(&w);
}

// ---------------------------------------------------------------------------
// LINK_* (PS-046, protocol.md 6.11)
// ---------------------------------------------------------------------------

u8 MpProto_LinkMaskCount(u8 mask)
{
    u8 count = 0;
    u32 i;

    for (i = 0; i < MP_LINK_MAX_MEMBERS; i++)
    {
        if (mask & (1 << i))
            count++;
    }
    return count;
}

// A member mask: only slots 0..3, and 2..4 members.
static bool8 MpIsValidLinkMask(u8 mask)
{
    return (mask & ~0x0F) == 0 && MpProto_LinkMaskCount(mask) >= 2;
}

bool8 MpProto_IsLinkCtlType(u8 type)
{
    switch (type)
    {
    case MP_MSG_LINK_JOIN:
    case MP_MSG_LINK_LEAVE:
    case MP_MSG_LINK_GROUP:
    case MP_MSG_LINK_END:
    case MP_MSG_LINK_OPEN:
    case MP_MSG_LINK_CLOSE:
    case MP_MSG_LINK_START:
    case MP_MSG_LINK_STATUS:
    case MP_MSG_LINK_BUS_START:
    case MP_MSG_LINK_BUS_END:
        return TRUE;
    default:
        return FALSE;
    }
}

u8 MpProto_ParseLinkCtl(u8 type, const u8 *payload, u32 len, struct MpMsgLinkCtl *out)
{
    struct MpMsgLinkCtl m;
    u8 result;

    memset(out, 0, sizeof(*out));
    if (!MpProto_IsLinkCtlType(type))
        return MP_PARSE_ERR_BAD_FIELD;
    result = MpCheckExactSize(payload, len, MP_LINK_CTL_SIZE);
    if (result != MP_PARSE_OK)
        return result;
    m.groupId = payload[0];
    m.arg1 = payload[1];
    m.arg2 = payload[2];
    // payload[3] is reserved

    switch (type)
    {
    case MP_MSG_LINK_JOIN:
        if (!MpIsValidLinkMask(m.arg1))
            return MP_PARSE_ERR_BAD_FIELD;
        m.groupId = 0;
        m.arg2 = 0;
        break;
    case MP_MSG_LINK_LEAVE:
        m.groupId = m.arg1 = m.arg2 = 0;
        break;
    case MP_MSG_LINK_GROUP:
        if (m.groupId == 0 || !MpIsValidLinkMask(m.arg1) || m.arg2 >= MpProto_LinkMaskCount(m.arg1))
            return MP_PARSE_ERR_BAD_FIELD;
        break;
    case MP_MSG_LINK_END:
        if (m.arg1 == 0)
            return MP_PARSE_ERR_BAD_FIELD;
        m.arg2 = 0;
        break;
    case MP_MSG_LINK_OPEN:
    case MP_MSG_LINK_CLOSE:
    case MP_MSG_LINK_START:
        if (m.groupId == 0)
            return MP_PARSE_ERR_BAD_FIELD;
        m.arg2 = 0;
        break;
    case MP_MSG_LINK_STATUS:
        if (m.groupId == 0 || (m.arg1 & ~0x0F) != 0)
            return MP_PARSE_ERR_BAD_FIELD;
        m.arg2 = 0;
        break;
    case MP_MSG_LINK_BUS_START:
        if (m.groupId == 0 || m.arg1 == 0)
            return MP_PARSE_ERR_BAD_FIELD;
        break;
    case MP_MSG_LINK_BUS_END:
        if (m.groupId == 0 || m.arg1 == 0)
            return MP_PARSE_ERR_BAD_FIELD;
        m.arg2 = 0;
        break;
    }
    *out = m;
    return MP_PARSE_OK;
}

u16 MpProto_WriteLinkCtl(u8 *buf, u16 cap, u8 type, const struct MpMsgLinkCtl *msg)
{
    struct MpWriter w;

    if (!MpProto_IsLinkCtlType(type))
        return 0;
    MpWriter_Init(&w, buf, cap);
    MpWriteHeader(&w, type, MP_PROTOCOL_VERSION, MP_LINK_CTL_SIZE);
    MpWriteU8(&w, msg->groupId);
    MpWriteU8(&w, msg->arg1);
    MpWriteU8(&w, msg->arg2);
    MpWriteU8(&w, 0);
    return MpFinishWrite(&w);
}

u8 MpProto_ParseLinkCmd(const u8 *payload, u32 len, struct MpMsgLinkCmd *out)
{
    struct MpMsgLinkCmd m;
    struct MpReader r;
    u16 any = 0;
    u8 result;
    u32 i;

    memset(out, 0, sizeof(*out));
    result = MpCheckExactSize(payload, len, MP_LINK_CMD_SIZE);
    if (result != MP_PARSE_OK)
        return result;
    MpReader_Init(&r, payload, len);
    m.groupId = MpReadU8(&r);
    m.busEpoch = MpReadU8(&r);
    MpReadU16(&r); // reserved
    for (i = 0; i < MP_LINK_CMD_WORDS; i++)
    {
        m.cmd[i] = MpReadU16(&r);
        any |= m.cmd[i];
    }
    if (r.error)
        return MP_PARSE_ERR_TRUNCATED;
    if (m.groupId == 0 || m.busEpoch == 0 || any == 0)
        return MP_PARSE_ERR_BAD_FIELD;
    *out = m;
    return MP_PARSE_OK;
}

u16 MpProto_WriteLinkCmd(u8 *buf, u16 cap, const struct MpMsgLinkCmd *msg)
{
    struct MpWriter w;
    u32 i;

    MpWriter_Init(&w, buf, cap);
    MpWriteHeader(&w, MP_MSG_LINK_CMD, MP_PROTOCOL_VERSION, MP_LINK_CMD_SIZE);
    MpWriteU8(&w, msg->groupId);
    MpWriteU8(&w, msg->busEpoch);
    MpWriteU16(&w, 0);
    for (i = 0; i < MP_LINK_CMD_WORDS; i++)
        MpWriteU16(&w, msg->cmd[i]);
    return MpFinishWrite(&w);
}

u8 MpProto_ParseLinkRow(const u8 *payload, u32 len, struct MpMsgLinkRow *out)
{
    struct MpMsgLinkRow m;
    struct MpReader r;
    u32 i, j;

    memset(out, 0, sizeof(*out));
    memset(&m, 0, sizeof(m));
    if (payload == NULL || len < MP_LINK_ROW_SIZE(0))
        return MP_PARSE_ERR_TRUNCATED;
    m.count = payload[2];
    if (m.count < 2 || m.count > MP_LINK_MAX_MEMBERS)
        return MP_PARSE_ERR_BAD_FIELD;
    if (len < (u32)MP_LINK_ROW_SIZE(m.count))
        return MP_PARSE_ERR_TRUNCATED;
    if (len > (u32)MP_LINK_ROW_SIZE(m.count))
        return MP_PARSE_ERR_OVERSIZED;
    MpReader_Init(&r, payload, len);
    m.groupId = MpReadU8(&r);
    m.busEpoch = MpReadU8(&r);
    MpReadU8(&r); // count, read above
    MpReadU8(&r); // reserved
    m.rowSeq = MpReadU16(&r);
    MpReadU16(&r); // reserved
    for (i = 0; i < m.count; i++)
    {
        for (j = 0; j < MP_LINK_CMD_WORDS; j++)
            m.cmds[i][j] = MpReadU16(&r);
    }
    if (r.error)
        return MP_PARSE_ERR_TRUNCATED;
    if (m.groupId == 0 || m.busEpoch == 0)
        return MP_PARSE_ERR_BAD_FIELD;
    *out = m;
    return MP_PARSE_OK;
}

u16 MpProto_WriteLinkRow(u8 *buf, u16 cap, const struct MpMsgLinkRow *msg)
{
    struct MpWriter w;
    u32 i, j;

    if (msg->count < 2 || msg->count > MP_LINK_MAX_MEMBERS)
        return 0;
    MpWriter_Init(&w, buf, cap);
    MpWriteHeader(&w, MP_MSG_LINK_ROW, MP_PROTOCOL_VERSION, MP_LINK_ROW_SIZE(msg->count));
    MpWriteU8(&w, msg->groupId);
    MpWriteU8(&w, msg->busEpoch);
    MpWriteU8(&w, msg->count);
    MpWriteU8(&w, 0);
    MpWriteU16(&w, msg->rowSeq);
    MpWriteU16(&w, 0);
    for (i = 0; i < msg->count; i++)
    {
        for (j = 0; j < MP_LINK_CMD_WORDS; j++)
            MpWriteU16(&w, msg->cmds[i][j]);
    }
    return MpFinishWrite(&w);
}
