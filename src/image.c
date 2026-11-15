// image.c
//
// Finds the game's image. Normally that's the process's own executable. A host that
// runs the app as a guest does it differently: LiveContainer turns the app into a dylib
// (MH_EXECUTE to MH_DYLIB, __PAGEZERO shrunk) and dlopens it inside its own process. Then
// _NSGetMachExecuteHeader() is the host's header, and the game is one of dyld's images.
// The game is recognised by the UUID of the build the patches are for, so either way the
// same code finds it.
//
// Everything that needs the game's addresses goes through here: the patches (hooks.c),
// the prepatch segments (prepatch.c) and Hyperspace (hyperspace.c).

#include "image.h"

#include <mach-o/dyld.h>
#include <stdbool.h>
#include <string.h>

// FTL for iPad 1.6.21, arm64 slice
static const uint8_t GAME_UUID[16] = {0x87, 0x3d, 0x2b, 0x34, 0xd7, 0x07, 0x3d, 0x6b,
                                      0xab, 0x83, 0x26, 0x7b, 0xf4, 0xc5, 0xbb, 0xc0};

static bool IsGame(const struct mach_header_64 *h)
{
    if (!h || h->magic != MH_MAGIC_64) return false;
    const uint8_t *lc = (const uint8_t *)(h + 1);
    for (uint32_t i = 0; i < h->ncmds; i++)
    {
        const struct load_command *c = (const void *)lc;
        if (c->cmdsize < 8) return false;
        if (c->cmd == LC_UUID) return !memcmp(((const struct uuid_command *)c)->uuid, GAME_UUID, 16);
        lc += c->cmdsize;
    }
    return false;
}

// The image's place in dyld's list; -1 when it isn't loaded
static int GameIndex(void)
{
    for (uint32_t i = 0; i < _dyld_image_count(); i++)
        if (IsGame((const struct mach_header_64 *)_dyld_get_image_header(i))) return (int)i;
    return -1;
}

const struct mach_header_64 *GameImage(void)
{
    int i = GameIndex();
    return i < 0 ? NULL : (const struct mach_header_64 *)_dyld_get_image_header((uint32_t)i);
}

const char *GameImagePath(void)
{
    int i = GameIndex();
    return i < 0 ? NULL : _dyld_get_image_name((uint32_t)i);
}

// The file's __TEXT starts at 0x100000000 in this build (game.h's addresses are from
// there), so that's what the loaded header is offset by.
intptr_t GameSlide(void)
{
    const struct mach_header_64 *h = GameImage();
    return h ? (intptr_t)h - (intptr_t)0x100000000ull : 0;
}
