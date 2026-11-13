#ifdef PORTABLE
#include "global.h"
#include "platform/dma.h"
#include "m4a.h"
#include "gba/flash_internal.h" // PS-070: FLASH_PC_SIZE

u16 INTR_CHECK;
void *INTR_VECTOR;
unsigned char REG_BASE[0x400] __attribute__ ((aligned (4)));
unsigned char PLTT[PLTT_SIZE] __attribute__ ((aligned (4)));
unsigned char VRAM_[VRAM_SIZE] __attribute__ ((aligned (4)));
unsigned char OAM[OAM_SIZE] __attribute__ ((aligned (4)));
unsigned char FLASH_BASE[FLASH_PC_SIZE] __attribute__ ((aligned (4))); // PS-070: 36 sectors (144 KB)
struct SoundInfo *SOUND_INFO_PTR;

extern void (*const gIntrTable[])(void);

void RunDMAsAndVBlank(void)
{
	REG_DISPSTAT |= INTR_FLAG_VBLANK;

	RunDMAs(DMA_HBLANK);

	if (REG_DISPSTAT & DISPSTAT_VBLANK_INTR)
		gIntrTable[4]();
	REG_DISPSTAT &= ~INTR_FLAG_VBLANK;
}

void AudioUpdate(void)
{
	if (gSoundInit == FALSE)
		return;

	m4aSoundMain();
	m4aSoundVSync();
}
#endif