// Bounded little-endian byte writer/reader (protocol.md section 2).
// All bounds checks are done with subtraction against `size`, never by adding
// to `pos`, so a large `len` cannot overflow.

#include <string.h>
#include "gba/types.h"
#include "gba/defines.h"
#include "mp/mp_wire.h"

void MpWriter_Init(struct MpWriter *w, u8 *data, u32 size)
{
    w->data = data;
    w->size = (data != NULL) ? size : 0;
    w->pos = 0;
    w->error = FALSE;
}

static bool8 MpWriterHasRoom(struct MpWriter *w, u32 len)
{
    if (w->error || w->pos > w->size || len > w->size - w->pos)
    {
        w->error = TRUE;
        return FALSE;
    }
    return TRUE;
}

void MpWriteU8(struct MpWriter *w, u8 value)
{
    if (!MpWriterHasRoom(w, 1))
        return;
    w->data[w->pos++] = value;
}

void MpWriteU16(struct MpWriter *w, u16 value)
{
    if (!MpWriterHasRoom(w, 2))
        return;
    w->data[w->pos++] = (u8)(value & 0xFF);
    w->data[w->pos++] = (u8)(value >> 8);
}

void MpWriteU32(struct MpWriter *w, u32 value)
{
    if (!MpWriterHasRoom(w, 4))
        return;
    w->data[w->pos++] = (u8)(value & 0xFF);
    w->data[w->pos++] = (u8)((value >> 8) & 0xFF);
    w->data[w->pos++] = (u8)((value >> 16) & 0xFF);
    w->data[w->pos++] = (u8)(value >> 24);
}

void MpWriteS16(struct MpWriter *w, s16 value)
{
    MpWriteU16(w, (u16)value);
}

void MpWriteBytes(struct MpWriter *w, const u8 *src, u32 len)
{
    if (len == 0)
        return;
    if (src == NULL || !MpWriterHasRoom(w, len))
    {
        w->error = TRUE;
        return;
    }
    memcpy(&w->data[w->pos], src, len);
    w->pos += len;
}

void MpReader_Init(struct MpReader *r, const u8 *data, u32 size)
{
    r->data = data;
    r->size = (data != NULL) ? size : 0;
    r->pos = 0;
    r->error = FALSE;
}

static bool8 MpReaderHas(struct MpReader *r, u32 len)
{
    if (r->error || r->pos > r->size || len > r->size - r->pos)
    {
        r->error = TRUE;
        return FALSE;
    }
    return TRUE;
}

u8 MpReadU8(struct MpReader *r)
{
    if (!MpReaderHas(r, 1))
        return 0;
    return r->data[r->pos++];
}

u16 MpReadU16(struct MpReader *r)
{
    u16 value;

    if (!MpReaderHas(r, 2))
        return 0;
    value = (u16)(r->data[r->pos] | (r->data[r->pos + 1] << 8));
    r->pos += 2;
    return value;
}

u32 MpReadU32(struct MpReader *r)
{
    u32 value;

    if (!MpReaderHas(r, 4))
        return 0;
    value = (u32)r->data[r->pos]
          | ((u32)r->data[r->pos + 1] << 8)
          | ((u32)r->data[r->pos + 2] << 16)
          | ((u32)r->data[r->pos + 3] << 24);
    r->pos += 4;
    return value;
}

s16 MpReadS16(struct MpReader *r)
{
    return (s16)MpReadU16(r);
}

void MpReadBytes(struct MpReader *r, u8 *dest, u32 len)
{
    if (len == 0)
        return;
    if (!MpReaderHas(r, len))
    {
        memset(dest, 0, len);
        return;
    }
    memcpy(dest, &r->data[r->pos], len);
    r->pos += len;
}

u32 MpReader_Remaining(const struct MpReader *r)
{
    if (r->error || r->pos > r->size)
        return 0;
    return r->size - r->pos;
}
