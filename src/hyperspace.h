// Hyperspace, when it's installed and the applied mods include it; see hyperspace.c.
#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

    void HyperspaceStart(void);  // before the game's code runs; exits if it fails
    bool HyperspaceRunning(void);  // started, and running
// Right clicks for mods' scripts: action is HS_RIGHT_BUTTON_*, x and y the game's
// screen pixels (the drawable's)
    bool HyperspaceWantsRightClick(void);
    void HyperspaceRightButton(int action, int x, int y);

#ifdef __cplusplus
}
#endif
