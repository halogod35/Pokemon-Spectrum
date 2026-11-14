#ifndef GUARD_GLOBAL_BERRY_H
#define GUARD_GLOBAL_BERRY_H

#define BERRY_NAME_LENGTH 6
#define BERRY_ITEM_EFFECT_COUNT 18

// PS-079 (BUG-015). struct Berry2 below is saved (SaveBlock1.enigmaBerry) and GetBerryInfo() casts
// it to struct Berry, so both have the same explicit layout: the two description pointers sit in
// 8-byte slots at 0x10 and 0x18 whatever the build, with explicit padding before and after, and
// sizeof is 0x28 everywhere (save.c asserts it).
struct Berry
{
    /*0x00*/ const u8 name[BERRY_NAME_LENGTH + 1];
    /*0x07*/ u8 firmness;
    /*0x08*/ u16 size;
    /*0x0A*/ u8 maxYield;
    /*0x0B*/ u8 minYield;
    /*0x0C*/ u32 padding1;
    /*0x10*/ union {
                 const u8 *description1;
                 u64 description1Slot;
             };
    /*0x18*/ union {
                 const u8 *description2;
                 u64 description2Slot;
             };
    /*0x20*/ u8 stageDuration;
    /*0x21*/ u8 spicy;
    /*0x22*/ u8 dry;
    /*0x23*/ u8 sweet;
    /*0x24*/ u8 bitter;
    /*0x25*/ u8 sour;
    /*0x26*/ u8 smoothness;
    /*0x27*/ u8 padding2;
};

// with no const fields

struct Berry2 // same layout as struct Berry (PS-079), see above
{
    /*0x00*/ u8 name[BERRY_NAME_LENGTH + 1];
    /*0x07*/ u8 firmness;
    /*0x08*/ u16 size;
    /*0x0A*/ u8 maxYield;
    /*0x0B*/ u8 minYield;
    /*0x0C*/ u32 padding1;
    /*0x10*/ union {
                 u8 *description1;
                 u64 description1Slot;
             };
    /*0x18*/ union {
                 u8 *description2;
                 u64 description2Slot;
             };
    /*0x20*/ u8 stageDuration;
    /*0x21*/ u8 spicy;
    /*0x22*/ u8 dry;
    /*0x23*/ u8 sweet;
    /*0x24*/ u8 bitter;
    /*0x25*/ u8 sour;
    /*0x26*/ u8 smoothness;
    /*0x27*/ u8 padding2;
};

struct EnigmaBerry
{
    struct Berry2 berry;
    u8 itemEffect[BERRY_ITEM_EFFECT_COUNT];
    u8 holdEffect;
    u8 holdEffectParam;
    u32 checksum;
};

struct BattleEnigmaBerry
{
    /*0x00*/ u8 name[BERRY_NAME_LENGTH + 1];
    /*0x07*/ u8 holdEffect;
    /*0x08*/ u8 itemEffect[BERRY_ITEM_EFFECT_COUNT];
    /*0x1A*/ u8 holdEffectParam;
};

struct BerryTree
{
    u8 berry;
    u8 stage:7;
    u8 stopGrowth:1;
    u16 minutesUntilNextStage;
    u8 berryYield;
    u8 regrowthCount:4;
    u8 watered1:1;
    u8 watered2:1;
    u8 watered3:1;
    u8 watered4:1;
} ALIGNED(4);

#endif // GUARD_GLOBAL_BERRY_H
