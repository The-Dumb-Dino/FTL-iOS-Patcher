// prepatch.c
//
// A prepatched game, for iOS: there the game's code can't be changed at runtime, so
// every change hooks.c and Hyperspace make is put into the game's file beforehand.
// The iOS patcher adds two segments to the game (patcher/src/prepatch.cpp):
//   __HSTEXT  code: this host's trampolines (HOST_CODE), Hyperspace's (the rest),
//             and at its end the hooked functions' first instructions (ORIGINALS)
//   __HSDATA  the pointers those trampolines jump through: this host's (HOST_SLOTS),
//             then Hyperspace's
// Everything in __HSTEXT reaches the game PC-relatively and the rest through
// __HSDATA, so it doesn't depend on where anything is loaded.
//
// Record (FTL_PREPATCH_RECORD=<file>, on a Mac, where code can be written): the
// game starts as usual up to the point where every change is made, then the
// changes are written to <file> and the process ends. Verify (the segments are
// there, nothing asked to record): the code is already in the file, so it's only
// compared, and the pointers are filled in.

#include "prepatch.h"

#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HOST_CODE  0x4000   // bytes of __HSTEXT for hooks.c
#define ORIGINALS  0x4000   // bytes at the end of __HSTEXT for Hyperspace's table
#define HOST_SLOTS 16      // pointers of __HSDATA for hooks.c

extern const struct mach_header_64 *_NSGetMachExecuteHeader(void);

static int gMode;
static uint8_t *gCode, *gData;
static size_t gCodeSize, gDataSize, gHostCodeUsed, gHostSlotsUsed;
static const char *gRecordPath;

static const struct segment_command_64 *FindSegment(const struct mach_header_64 *h, const char *name)
{
    const uint8_t *lc = (const uint8_t *)(h + 1);
    for (uint32_t i = 0; i < h->ncmds; i++)
    {
        const struct segment_command_64 *s = (const void *)lc;
        if (s->cmd == LC_SEGMENT_64 && !strncmp(s->segname, name, 16)) return s;
        lc += s->cmdsize;
    }
    return NULL;
}

void PrepatchInit(intptr_t slide)
{
    const struct mach_header_64 *h = _NSGetMachExecuteHeader();
    const struct segment_command_64 *code = FindSegment(h, "__HSTEXT"), *data = FindSegment(h, "__HSDATA");
    if (!code || !data) return;
    gCode = (uint8_t *)(code->vmaddr + slide);
    gCodeSize = code->vmsize;
    gData = (uint8_t *)(data->vmaddr + slide);
    gDataSize = data->vmsize;
    if (gCodeSize <= HOST_CODE + ORIGINALS || gDataSize <= HOST_SLOTS * sizeof(void *))
    {
        fprintf(stderr, "[ftlios] the prepatch segments are too small; ignoring them\n");
        return;
    }
    gRecordPath = getenv("FTL_PREPATCH_RECORD");
    gMode = gRecordPath ? PREPATCH_RECORD : PREPATCH_VERIFY;
    fprintf(stderr, "[ftlios] prepatched game: %s\n", gMode == PREPATCH_RECORD ? "recording" : "verifying");
}

int PrepatchMode(void)
{
    return gMode;
}

// Room in the host's part of __HSTEXT and __HSDATA, handed out in the same order every time
void *PrepatchHostCode(size_t n)
{
    n = (n + 15) & ~(size_t)15;
    if (gHostCodeUsed + n > HOST_CODE) return NULL;
    void *p = gCode + gHostCodeUsed;
    gHostCodeUsed += n;
    return p;
}

void **PrepatchHostSlot(void)
{
    if (gHostSlotsUsed >= HOST_SLOTS) return NULL;
    return (void **)gData + gHostSlotsUsed++;
}

void PrepatchHyperspaceAreas(void **code, size_t *codeSize, void ***slots, size_t *slotCount, void **originals,
                             size_t *originalsSize)
{
    *code = gCode + HOST_CODE;
    *codeSize = gCodeSize - HOST_CODE - ORIGINALS;
    *slots = (void **)gData + HOST_SLOTS;
    *slotCount = gDataSize / sizeof(void *) - HOST_SLOTS;
    *originals = gCode + gCodeSize - ORIGINALS;
    *originalsSize = ORIGINALS;
}

// Record: the words of the game's __text that differ from its file, and all of
// __HSTEXT. File format (little-endian): "FTLPATCH", u32 version (1), u32 word
// count, u32 __HSTEXT size, then per word u32 offset (from the image's start) and
// u32 value, then __HSTEXT.
void PrepatchRecordFinish(void)
{
    if (gMode != PREPATCH_RECORD) return;
    const struct mach_header_64 *h = _NSGetMachExecuteHeader();
    const struct segment_command_64 *text = FindSegment(h, "__TEXT");
    const struct section_64 *sect = (const void *)(text + 1), *codeSect = NULL;
    for (uint32_t i = 0; i < text->nsects; i++)
        if (!strncmp(sect[i].sectname, "__text", 16)) codeSect = &sect[i];
    char exe[4096];
    uint32_t n = sizeof exe;
    FILE *in = _NSGetExecutablePath(exe, &n) ? NULL : fopen(exe, "rb");
    uint8_t *disk = codeSect ? malloc(codeSect->size) : NULL;
    if (!in || !disk || fseek(in, codeSect->offset, SEEK_SET) || fread(disk, 1, codeSect->size, in) != codeSect->size)
    {
        fprintf(stderr, "[ftlios] prepatch: can't read the game's file\n");
        exit(1);
    }
    fclose(in);
    const uint8_t *mem = (const uint8_t *)h + (codeSect->addr - text->vmaddr);
    FILE *out = fopen(gRecordPath, "wb");
    if (!out)
    {
        fprintf(stderr, "[ftlios] prepatch: can't write %s\n", gRecordPath);
        exit(1);
    }
    uint32_t count = 0;
    for (uint64_t i = 0; i + 4 <= codeSect->size; i += 4) count += memcmp(mem + i, disk + i, 4) != 0;
    uint32_t head[3] = {1, count, (uint32_t)gCodeSize};
    fwrite("FTLPATCH", 1, 8, out);
    fwrite(head, 4, 3, out);
    for (uint64_t i = 0; i + 4 <= codeSect->size; i += 4)
    {
        if (!memcmp(mem + i, disk + i, 4)) continue;
        uint32_t e[2] = {(uint32_t)(codeSect->addr - text->vmaddr + i)};
        memcpy(&e[1], mem + i, 4);
        fwrite(e, 4, 2, out);
    }
    fwrite(gCode, 1, gCodeSize, out);
    fclose(out);
    free(disk);
    fprintf(stderr, "[ftlios] prepatch recorded: %u changed words in __text, %zuK of __HSTEXT, to %s\n", count,
            gCodeSize >> 10, gRecordPath);
    exit(0);
}
