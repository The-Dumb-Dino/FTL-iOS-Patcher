// Where the game's own image is, see image.c.
#pragma once
#include <mach-o/loader.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    // The game's image: the process's executable, or (a host like LiveContainer runs the
    // app as a guest, loaded with dlopen) one of dyld's images. Found by its UUID.
    const struct mach_header_64 *GameImage(void);  // NULL if this isn't the game
    const char *GameImagePath(void);               // the file it was loaded from; NULL if unknown
    intptr_t GameSlide(void);                      // the game's addresses minus the file's (game.h's base)

#ifdef __cplusplus
}
#endif
