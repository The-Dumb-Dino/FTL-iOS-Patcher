// display.c
//
// Phones. The game makes its view the screen's size and draws into it at the view's
// contentScaleFactor; its layout is 768 units tall and only scales up from 1536
// pixels (an iPad's retina screen). A phone has fewer (an iPhone Pro Max 1320), so
// the game drew a small 4:3 picture in the middle at one pixel per unit.
//
// SILView gets its own setContentScaleFactor:, which raises the factor until the
// view is 1536 pixels tall: the game then lays out at two pixels per unit and iOS
// scales the picture down to the screen. Touches are unaffected (the game takes
// them relative to the view). iPads and Macs already have the pixels: no change.
// No code is patched, so a prepatched game needs nothing for this.

#include "display.h"

#include <objc/message.h>
#include <objc/runtime.h>
#include <stdio.h>

#define MIN_PIXELS 1536.0  // tall: the game's layout at 2x

typedef struct
{
    double x, y, w, h;
} Rect;

static void (*gSuperSetScale)(id, SEL, double);

static void SetScale(id self, SEL cmd, double scale)
{
    Rect b = ((Rect (*)(id, SEL))objc_msgSend)(self, sel_registerName("bounds"));
    double tall = b.w < b.h ? b.w : b.h;  // landscape: the shorter side
    if (tall > 0 && tall * scale < MIN_PIXELS)
    {
        double want = MIN_PIXELS / tall;
        fprintf(stderr, "[ftlios] screen %.0fx%.0f points at %.2fx: drawing at %.2fx\n", b.w, b.h, scale, want);
        scale = want;
    }
    gSuperSetScale(self, cmd, scale);
}

void DisplayInstall(void)
{
    Class view = objc_getClass("SILView"), uiview = objc_getClass("UIView");
    SEL sel = sel_registerName("setContentScaleFactor:");
    if (!view || !uiview)
    {
        fprintf(stderr, "[ftlios] no SILView; the picture won't be scaled for phones\n");
        return;
    }
    gSuperSetScale = (void (*)(id, SEL, double))class_getMethodImplementation(uiview, sel);
    if (!class_addMethod(view, sel, (IMP)SetScale, "v@:d"))
        fprintf(stderr, "[ftlios] SILView already has setContentScaleFactor:; not scaling for phones\n");
}
