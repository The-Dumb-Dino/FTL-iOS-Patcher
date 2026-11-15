// gamedata.h
//
// The iPad mod fix-up for a copy of ftl.dat, see gamedata.c.
#pragma once

#ifdef __cplusplus
extern "C"
{
#endif

    enum
    {
        GAME_DATA_ERROR = -1,  // not an ftl.dat, or couldn't be read or written; the file is as it was
        GAME_DATA_UNCHANGED = 0,
        GAME_DATA_CHANGED = 1,
    };

    int GameDataFix(const char *path);

#ifdef __cplusplus
}
#endif
