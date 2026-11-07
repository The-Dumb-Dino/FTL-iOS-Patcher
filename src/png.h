// PNG support for the iPad build, which was compiled without it (see png.c).
#pragma once
#include <stdint.h>

// Takes the place of the game's PngParse (G_PNG_PARSE)
void *PngParse(const void *data, uint32_t size, int memFlags, int *wRet, int *hRet);
