// A prepatched game, for iOS: see prepatch.c.
#pragma once
#include <stddef.h>
#include <stdint.h>

enum
{
    PREPATCH_NONE = 0,
    PREPATCH_RECORD = 1,
    PREPATCH_VERIFY = 2
};  // as HS_PREPATCH_*

void PrepatchInit(intptr_t slide);  // finds __HSTEXT and __HSDATA, picks the mode
int PrepatchMode(void);
void *PrepatchHostCode(size_t n);  // in __HSTEXT, for hooks.c's trampolines
void **PrepatchHostSlot(void);     // in __HSDATA, a pointer they jump through
void PrepatchHyperspaceAreas(void **code, size_t *codeSize, void ***slots, size_t *slotCount, void **originals,
                             size_t *originalsSize);
void PrepatchRecordFinish(void);   // record mode: writes the patch and ends the process
