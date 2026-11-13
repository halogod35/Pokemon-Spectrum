#ifndef GUARD_GBA_FLASH_INTERNAL_H
#define GUARD_GBA_FLASH_INTERNAL_H

// PS-070: the PC flash is SECTORS_COUNT (save.h) sectors of 4 KB: 36 sectors, 144 KB.
// save.c checks that the two agree. Format 1 files (128 KB) are migrated on load.
#define FLASH_PC_SECTORS 36
#define FLASH_PC_SIZE    (FLASH_PC_SECTORS * 4096)
#define FLASH_PC_V1_SIZE (32 * 4096) // save format 1 (v0.2.0 and earlier) file size, = SAVE_V1_FLASH_SIZE

#ifndef PORTABLE
#define FLASH_BASE ((u8 *)0xE000000)
#else
extern unsigned char FLASH_BASE[FLASH_PC_SIZE];
#endif

#define FLASH_WRITE(addr, data) ((*(vu8 *)(FLASH_BASE + (addr))) = (data))

#define FLASH_ROM_SIZE_1M 131072 // 1 megabit ROM

#define SECTORS_PER_BANK 16

struct FlashSector
{
    u32 size;
    u8 shift;
    u16 count;
    u16 top;
};

struct FlashType {
    u32 romSize;
    struct FlashSector sector;
    u16 wait[2]; // game pak bus read/write wait

    // TODO: add support for anonymous unions/structs if possible
    union {
        struct {
        u8 makerId;
        u8 deviceId;
        } separate;
        u16 joined;
    } ids;
};

struct FlashSetupInfo
{
    u16 (*programFlashByte)(u16, u32, u8);
    u16 (*programFlashSector)(u16, u8 *);
    u16 (*eraseFlashChip)(void);
    u16 (*eraseFlashSector)(u16);
    u16 (*WaitForFlashWrite)(u8, u8 *, u8);
    const u16 *maxTime;
    struct FlashType type;
};

extern const struct FlashSetupInfo DUMMY_SAVE;

extern u16 gFlashNumRemainingBytes;

extern u16 (*ProgramFlashByte)(u16, u32, u8);
extern u16 (*ProgramFlashSector)(u16, u8 *);
extern u16 (*EraseFlashChip)(void);
extern u16 (*EraseFlashSector)(u16);
extern u16 (*WaitForFlashWrite)(u8, u8 *, u8);
extern const u16 *gFlashMaxTime;
extern const struct FlashType *gFlash;

extern u8 (*PollFlashStatus)(u8 *);
extern u8 gFlashTimeoutFlag;

extern const struct FlashSetupInfo DUMMY_FLASH;
extern const struct FlashSetupInfo MX29L010;
extern const struct FlashSetupInfo LE26FV10N1TS;
extern const struct FlashSetupInfo DefaultFlash;

void SwitchFlashBank(u8 bankNum);
u16 ReadFlashId(void);
void StartFlashTimer(u8 phase);
void SetReadFlash1(u16 *dest);
void StopFlashTimer(void);
void ReadFlash(u16 sectorNum, u32 offset, u8 *dest, u32 size);

u16 WaitForFlashWrite_Common(u8 phase, u8 *addr, u8 lastData);

u16 EraseFlashChip_MX(void);
u16 EraseFlashSector_MX(u16 sectorNum);
u16 ProgramFlashByte_MX(u16 sectorNum, u32 offset, u8 data);
u16 ProgramFlashSector_MX(u16 sectorNum, u8 *src);

u16 WaitForFlashWrite_DUMMY(u8 phase, u8 *addr, u8 lastData);
u16 EraseFlashChip_DUMMY(void);
u16 EraseFlashSector_DUMMY(u16 sectorNum);
u16 ProgramFlashByte_DUMMY(u16 sectorNum, u32 offset, u8 data);
u16 ProgramFlashSector_DUMMY(u16 sectorNum, void *src);

#endif // GUARD_GBA_FLASH_INTERNAL_H
