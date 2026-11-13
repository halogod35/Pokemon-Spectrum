#ifdef PORTABLE
#include "global.h"
#include "gba/gba.h"
#include "gba/flash_internal.h"
#include <stdio.h>

const u16 dummyMaxTime[] =
{
      10, 65469, TIMER_ENABLE | TIMER_INTR_ENABLE | TIMER_256CLK,
      10, 65469, TIMER_ENABLE | TIMER_INTR_ENABLE | TIMER_256CLK,
    2000, 65469, TIMER_ENABLE | TIMER_INTR_ENABLE | TIMER_256CLK,
    2000, 65469, TIMER_ENABLE | TIMER_INTR_ENABLE | TIMER_256CLK,
};

const struct FlashSetupInfo DUMMY_SAVE =
{
    ProgramFlashByte_DUMMY,
    ProgramFlashSector_DUMMY,
    EraseFlashChip_DUMMY,
    EraseFlashSector_DUMMY,
    WaitForFlashWrite_DUMMY,
    dummyMaxTime,
    {
        FLASH_PC_SIZE, // ROM size (PS-070: 36 sectors, 144 KB)
        {
            4096, // sector size
              12, // bit shift to multiply by sector size (4096 == 1 << 12)
            FLASH_PC_SECTORS, // number of sectors
               0  // appears to be unused
        },
        { 3, 1 }, // wait state setup data
        { { 0xCC, 0xCC } } // ID
    }
};

u16 WaitForFlashWrite_DUMMY(u8 phase, u8 *addr, u8 lastData)
{
    // stub
    return 0;
}

u16 EraseFlashChip_DUMMY(void)
{
    memset(FLASH_BASE, 0xFF, sizeof(FLASH_BASE));
    return 0;
}

u16 EraseFlashSector_DUMMY(u16 sectorNum)
{
    u8 clearBuffer[0x1000];

    memset(clearBuffer, 0xFF, sizeof(clearBuffer)); // MP: BUG-003 review. "= { 0xFF }" only set byte 0; erased flash is all 0xFF
    return ProgramFlashSector_DUMMY(sectorNum, &clearBuffer[0]);
}

u16 ProgramFlashByte_DUMMY(u16 sectorNum, u32 offset, u8 data)
{
    FLASH_BASE[(sectorNum << gFlash->sector.shift) + offset] = data;
    return 0;
}


u16 ProgramFlashSector_DUMMY(u16 sectorNum, void *src)
{
    memcpy(&FLASH_BASE[sectorNum << gFlash->sector.shift], src, 0x1000);
    return 0;
}
#endif //PORTABLE