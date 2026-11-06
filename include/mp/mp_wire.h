#ifndef GUARD_MP_MP_WIRE_H
#define GUARD_MP_MP_WIRE_H

// Bounded little-endian byte writer/reader (protocol.md section 2). Internal to src/mp.
// Both have a sticky error flag: once an access would go past the end, `error`
// is set, nothing more is written, and every further read returns 0.
// Never send raw structs: serialize field by field with these.

#include "gba/types.h"

struct MpWriter
{
    u8 *data;
    u32 size;
    u32 pos;
    bool8 error;
};

struct MpReader
{
    const u8 *data;
    u32 size;
    u32 pos;
    bool8 error;
};

void MpWriter_Init(struct MpWriter *w, u8 *data, u32 size);
void MpWriteU8(struct MpWriter *w, u8 value);
void MpWriteU16(struct MpWriter *w, u16 value);
void MpWriteU32(struct MpWriter *w, u32 value);
void MpWriteS16(struct MpWriter *w, s16 value);
void MpWriteBytes(struct MpWriter *w, const u8 *src, u32 len);

void MpReader_Init(struct MpReader *r, const u8 *data, u32 size);
u8 MpReadU8(struct MpReader *r);
u16 MpReadU16(struct MpReader *r);
u32 MpReadU32(struct MpReader *r);
s16 MpReadS16(struct MpReader *r);
// Copies exactly `len` bytes into `dest` (which must hold `len` bytes), or zero-fills
// `dest` and sets the error flag if fewer than `len` bytes remain.
void MpReadBytes(struct MpReader *r, u8 *dest, u32 len);
// Bytes not yet read (0 once the error flag is set).
u32 MpReader_Remaining(const struct MpReader *r);

#endif // GUARD_MP_MP_WIRE_H
