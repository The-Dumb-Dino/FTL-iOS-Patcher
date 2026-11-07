// touch.m
//
// iOS only: touch controls for small screens, after ftl-linux's UX layer (ux.c), which
// the Android app has too. All through the Objective-C runtime and the game's data:
// no code is patched, so a prepatched game needs nothing for it.
//  - Pinch to zoom, drag with two fingers to pan. The game's view is scaled inside a
//    host view; it reads touches with locationInView: on itself, so they stay right.
//    While zoomed in, a minimap in the corner shows where the view is; tap it to zoom
//    back out.
//  - A two-finger tap pauses and resumes.
//  - Hold a finger to see a magnifier above it (automatically on phones). Its picture
//    is read from the game's frame as SILView presents it.
//  - The beacon map and the upgrade screen zoom in by themselves on phones; events
//    too, as an option. Event text can be larger.
//  - A finger held still and a tap with another is a right click, for Hyperspace mods
//    whose scripts listen for one.
//  - Optionally, the saved run continues right away at launch.
// Settings (settings.m) change these; they're kept in NSUserDefaults.

#define GLES_SILENCE_DEPRECATION
#import <UIKit/UIKit.h>
#import <OpenGLES/ES3/gl.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <pthread.h>
#include <sys/utsname.h>

#import "ios.h"
#include "game.h"
#include "hooks.h"
#include "hyperspace.h"
#include "hyperspace_host.h"

// ---------------------------------------------------------------------------------
// options

NSString *const kTouchMagnifier = @"ftl.magnifier";        // 0 automatic (phones), 1 always, 2 off
NSString *const kTouchTwoFingerPause = @"ftl.two_finger_pause";
NSString *const kTouchPinchZoom = @"ftl.pinch_zoom";
NSString *const kTouchMinimap = @"ftl.minimap";
NSString *const kTouchWindowZoom = @"ftl.window_zoom";     // 0 automatic (phones), 1 always, 2 off
NSString *const kTouchEventZoom = @"ftl.event_zoom";
NSString *const kTouchEventText = @"ftl.event_text";       // 0 the game's, 1 large, 2 larger
NSString *const kTouchResumeRun = @"ftl.resume_run";
NSString *const kTouchHaptics = @"ftl.haptics";
NSString *const kTouchPortrait = @"ftl.portrait";          // the game turns upright too (default: the iPhone Duo)

static BOOL IsPhone(void)
{
    return UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPhone;
}

// The iPhone Duo, whose big screen makes upright play worthwhile (iPhone19,4).
BOOL TouchIsIphoneDuo(void)
{
    static int duo = -1;
    if (duo < 0)
    {
        const char *sim = getenv("SIMULATOR_MODEL_IDENTIFIER");
        struct utsname u;
        uname(&u);
        duo = !strcmp(sim ? sim : u.machine, "iPhone19,4");
    }
    return duo;
}

BOOL TouchPortraitAllowed(void)
{
    id v = [NSUserDefaults.standardUserDefaults objectForKey:kTouchPortrait];
    return v ? [v boolValue] : TouchIsIphoneDuo();
}

static BOOL OptBool(NSString *key, BOOL dflt)
{
    id v = [NSUserDefaults.standardUserDefaults objectForKey:key];
    return v ? [v boolValue] : dflt;
}
static NSInteger OptInt(NSString *key)
{
    return [NSUserDefaults.standardUserDefaults integerForKey:key];
}

static BOOL MagnifierOn(void)
{
    NSInteger m = OptInt(kTouchMagnifier);
    return m == 1 || (m == 0 && IsPhone());
}
static BOOL WindowZoomOn(void)
{
    NSInteger m = OptInt(kTouchWindowZoom);
    return m == 1 || (m == 0 && IsPhone());
}

static void Haptic(UIImpactFeedbackStyle style)
{
    if (!OptBool(kTouchHaptics, YES)) return;
    UIImpactFeedbackGenerator *g = [[UIImpactFeedbackGenerator alloc] initWithStyle:style];
    [g impactOccurred];
}

// ---------------------------------------------------------------------------------
// game state

static uint8_t *Capp(void)
{
    return HooksActive() ? *(uint8_t **)GameAddr(G_CAPP_PTR) : NULL;
}

static uint8_t *CommandGui(void)
{
    uint8_t *a = Capp();
    return a && !a[CAPP_MENU] && !a[CAPP_STARTUP] ? *(uint8_t **)(a + CAPP_GUI) : NULL;
}

static BOOL AtTopMenu(void)
{
    uint8_t *a = Capp();
    if (!a || !a[CAPP_MENU] || a[CAPP_STARTUP]) return NO;
    static const int subscreens[] = MENU_SUBSCREEN_OFFSETS;
    for (size_t i = 0; i < sizeof subscreens / sizeof *subscreens; i++)
        if (a[CAPP_MENU + subscreens[i]]) return NO;
    return YES;
}

// Like the desktop game's pause key. The new state, -1 when not in a run.
static int TogglePause(void)
{
    uint8_t *gui = CommandGui();
    if (!gui) return -1;
    ((void (*)(void *))GameAddr(G_TOGGLE_PAUSE))(gui);
    return gui[GUI_PAUSED] != 0;
}

// ---------------------------------------------------------------------------------
// the host view: the game's view, zoomed, with the magnifier and minimap on top

@interface FTLTouchHost : UIView
@property(nonatomic, strong) UIView *clip, *game;
@property(nonatomic, strong) UIView *loupe, *minimap, *minimapView;
@property(nonatomic) CGFloat zoom, x0, y0;  // zoom >= 1; the shown part's top left, in view points
- (void)setZoom:(CGFloat)z x0:(CGFloat)x y0:(CGFloat)y animated:(NSTimeInterval)duration;
@end

@interface FTLTouchHostController : UIViewController
@end
@implementation FTLTouchHostController
- (BOOL)prefersStatusBarHidden
{
    return YES;
}
- (BOOL)prefersHomeIndicatorAutoHidden
{
    return YES;
}
- (UIRectEdge)preferredScreenEdgesDeferringSystemGestures
{
    return UIRectEdgeAll;
}
// Landscape, like the game's own controller, unless upright play is turned on (Settings).
- (UIInterfaceOrientationMask)supportedInterfaceOrientations
{
    if (TouchPortraitAllowed()) return UIInterfaceOrientationMaskAll;
    UIViewController *game = self.childViewControllers.firstObject;
    return game ? game.supportedInterfaceOrientations : UIInterfaceOrientationMaskLandscape;
}
@end

static FTLTouchHost *gHost;
// The setting changed: let iOS ask again.
void TouchOrientationChanged(void)
{
    UIViewController *vc = gHost.window.rootViewController;
    if (@available(iOS 16.0, *)) [vc setNeedsUpdateOfSupportedInterfaceOrientations];
}
static const CGFloat ZOOM_MAX = 3.0;

@implementation FTLTouchHost
- (void)layoutSubviews
{
    [super layoutSubviews];
    self.clip.frame = self.bounds;
    [self apply];
    // Minimap: the screen's shape, top right, clear of the notch
    CGSize s = self.bounds.size;
    CGFloat w = 110, h = s.width > 0 ? w * s.height / s.width : 0;
    UIEdgeInsets safe = self.window.safeAreaInsets;
    self.minimap.frame = CGRectMake(s.width - w - MAX(12, safe.right), 12, w, h);
}
- (void)apply
{
    CGSize s = self.clip.bounds.size;
    CGFloat z = MAX(self.zoom, 1);
    self.x0 = MAX(0, MIN(self.x0, s.width - s.width / z));
    self.y0 = MAX(0, MIN(self.y0, s.height - s.height / z));
    self.game.transform = CGAffineTransformIdentity;
    self.game.frame = CGRectMake(0, 0, s.width, s.height);
    CGFloat tx = -s.width / 2 - z * (self.x0 - s.width / 2), ty = -s.height / 2 - z * (self.y0 - s.height / 2);
    self.game.transform = CGAffineTransformMake(z, 0, 0, z, tx, ty);
    BOOL zoomed = z > 1.01;
    self.minimap.hidden = !zoomed || !OptBool(kTouchMinimap, YES);
    CGRect m = self.minimap.bounds;
    self.minimapView.frame = CGRectMake(m.size.width * self.x0 / s.width, m.size.height * self.y0 / s.height,
                                        m.size.width / z, m.size.height / z);
}
- (void)setZoom:(CGFloat)z x0:(CGFloat)x y0:(CGFloat)y animated:(NSTimeInterval)duration
{
    void (^change)(void) = ^{
        self.zoom = MIN(ZOOM_MAX, MAX(1, z));
        self.x0 = x;
        self.y0 = y;
        [self apply];
    };
    if (duration > 0)
        [UIView animateWithDuration:duration
                              delay:0
                            options:UIViewAnimationOptionCurveEaseInOut | UIViewAnimationOptionBeginFromCurrentState
                         animations:change
                         completion:nil];
    else
        change();
}
@end

static void ResetZoom(BOOL animated)
{
    [gHost setZoom:1 x0:0 y0:0 animated:animated ? 0.18 : 0];
}

// Zoom by `factor`, keeping the game point under `p` (host points) where it is.
static void ZoomAt(CGFloat factor, CGPoint p, NSTimeInterval anim)
{
    CGFloat z = MAX(gHost.zoom, 1), nz = MIN(ZOOM_MAX, MAX(1, z * factor));
    CGFloat gx = gHost.x0 + p.x / z, gy = gHost.y0 + p.y / z;
    [gHost setZoom:nz x0:gx - p.x / nz y0:gy - p.y / nz animated:anim];
}

// ---------------------------------------------------------------------------------
// automatic zoom on windows (on the game's state, checked ten times a second)

static struct
{
    BOOL active, playerMoved;
    CGFloat zoom, x0, y0;     // the view to go back to
    CFTimeInterval closedAt;  // the window has been shut since (0: it's open)
} gAz;

static void ViewMovedByPlayer(void)
{
    if (gAz.active) gAz.playerMoved = YES;
}

// The open window's useful part, in HUD units (the iPad's 1024 x 768 layout, centred
// on the game's wider screen); top: an event, kept in view from its top. As in ftl-linux.
static BOOL WindowRect(uint8_t *gui, CGRect *r, BOOL *top)
{
    static const int modals[] = GUI_MODAL_OFFSETS;
    for (size_t i = 0; i < sizeof modals / sizeof *modals; i++)
        if (gui[modals[i] + 8]) return NO;
    uint8_t *open = NULL;
    uint8_t **fw = (uint8_t **)(gui + GUI_FOCUS_WINDOWS);
    for (uint8_t **w = (uint8_t **)fw[0]; w < (uint8_t **)fw[1] && !open; w++)
        if (*w && (*w)[8]) open = *w;
    *top = NO;
    if (open == gui + GUI_CHOICE)
    {
        uint8_t *box = open;
        const int *frame = *(int **)(box + CHOICE_FRAME);
        if (!frame) return NO;
        uint8_t **list = (uint8_t **)(box + CHOICE_LIST);
        if ((size_t)(list[1] - list[0]) / 584 > *(uint32_t *)(box + CHOICE_COLUMN)) return NO;
        double x = (box[CHOICE_CENTRED] ? 199 : 49) + frame[0] - 7, y = 149 + frame[1] - 7;
        const int32_t *const *rects = (const int32_t *const *)(box + CHOICE_RECTS);
        double last = 0;
        for (const int32_t *c = rects[0]; c && c + 4 <= rects[1]; c += 4)
            if (c[3] > 0 && c[1] + c[3] > last) last = c[1] + c[3];
        if (last <= y) return NO;
        double bottom = fmin(y + frame[3] + 14, last + 12);
        *r = CGRectMake(x, y, frame[2] + 14, bottom - y);
        *top = YES;
        return frame[2] > 0;
    }
    if (open == gui + GUI_UPGRADES)
    {
        if (*(uint32_t *)(open + 0x68) == 1) return NO;  // the crew tab: little to gain
        *r = CGRectMake(46, 85, 931, 509);
        return YES;
    }
    uint8_t *map = *(uint8_t **)(gui + GUI_STARMAP);
    if (open && open == map && !map[0x900])
    {  // the beacon map, not the sector map
        int x = *(int *)(map + 0x88), y = *(int *)(map + 0x8c);
        *r = CGRectMake(x + 1, y + 1, 750, 579);
        return YES;
    }
    return NO;
}

static BOOL gFingersDown;

static void AutozoomTick(void)
{
    uint8_t *gui = CommandGui();
    CGRect r;
    BOOL top;
    if (gui && WindowRect(gui, &r, &top) && (top ? OptBool(kTouchEventZoom, NO) : WindowZoomOn()))
    {
        gAz.closedAt = 0;
        if (gAz.playerMoved || gFingersDown) return;
        static CGRect last;  // an event is laid out once drawn: wait for two equal readings
        BOOL settled = CGRectEqualToRect(r, last);
        last = r;
        if (!settled) return;
        // HUD units -> view points: the HUD is 768 units tall, centred across
        CGSize s = gHost.clip.bounds.size;
        CGFloat k = s.height / 768.0, hudX = (s.width / k - 1024) / 2;
        CGRect v = CGRectMake((r.origin.x + hudX) * k, r.origin.y * k, r.size.width * k, r.size.height * k);
        CGFloat m = 14 * k;
        CGFloat z = MIN(s.width / (v.size.width + 2 * m), s.height / (v.size.height + 2 * m));
        if (z > 2.2) z = 2.2;  // the 2x picture gets soft beyond that
        if (z < 1.15) return;
        CGFloat vw = s.width / z, vh = s.height / z;
        CGFloat x0 = CGRectGetMidX(v) - vw / 2, y0 = top ? v.origin.y - m : CGRectGetMidY(v) - vh / 2;
        if (fabs(gHost.zoom - z) < 0.01 && fabs(gHost.x0 - MAX(0, MIN(x0, s.width - vw))) < 1 &&
            fabs(gHost.y0 - MAX(0, MIN(y0, s.height - vh))) < 1)
            return;
        if (!gAz.active)
        {
            gAz.active = YES;
            gAz.zoom = gHost.zoom;
            gAz.x0 = gHost.x0;
            gAz.y0 = gHost.y0;
        }
        [gHost setZoom:z x0:x0 y0:y0 animated:0.25];
        return;
    }
    if (!gAz.active) return;
    CFTimeInterval t = CACurrentMediaTime();
    if (!gAz.closedAt) gAz.closedAt = t;
    if (t - gAz.closedAt < 0.2 && gui) return;  // one event often follows another
    if (!gAz.playerMoved) [gHost setZoom:gAz.zoom x0:gAz.x0 y0:gAz.y0 animated:0.25];
    gAz.active = gAz.playerMoved = NO;
    gAz.closedAt = 0;
}

// Event text size, as in ftl-linux: the game's own size (10; 9 in Japanese), 12 or 13
static void EventFontTick(void)
{
    static int set, gameSize;
    uint8_t *gui = CommandGui();
    if (!gui) return;
    int *size = (int *)(gui + GUI_CHOICE + CHOICE_FONT);
    if (!gameSize)
    {
        if (*size < 9 || *size > 13) return;
        gameSize = *size;
    }
    NSInteger opt = OptInt(kTouchEventText);
    int want = opt == 0 ? (set ? gameSize : 0) : gameSize < 10 || opt == 1 ? 12 : 13;
    if (want && *size != want) *size = want;
    set = opt ? want : 0;
}

// Continue the saved run at launch: once, when the title screen first shows
static void ResumeRunTick(void)
{
    static BOOL done;
    static CFTimeInterval since;
    uint8_t *a = Capp();
    if (done || !a || a[CAPP_STARTUP]) return;
    if (!a[CAPP_MENU] || !AtTopMenu())
    {
        done = !a[CAPP_MENU];
        since = 0;
        return;
    }
    CFTimeInterval t = CACurrentMediaTime();
    if (!since) since = t;
    if (t - since < 0.5) return;  // the menu marks Continue active on its first frames
    done = YES;
    uint8_t *menu = a + CAPP_MENU;
    if (OptBool(kTouchResumeRun, NO) && menu[MENU_CONTINUE_ACTIVE]) *(int32_t *)(menu + MENU_ITEM) = 9;
}

// ---------------------------------------------------------------------------------
// the magnifier

#define LOUPE_SIZE 120.0  // points
#define LOUPE_ZOOM 2.0    // compared to how big the game is on screen

static pthread_mutex_t gLoupeLock = PTHREAD_MUTEX_INITIALIZER;
static BOOL gLoupeWant;   // read the game's frame for it
static CGPoint gLoupeAt;  // in the game's view, points
static CGFloat gLoupeSpan; // points of the game's view it shows across
static BOOL gLoupePending;
static void (*gPresent)(id, SEL);

// Game's thread, as SILView presents a frame: the part under the finger.
static void LoupeRead(UIView *view)
{
    pthread_mutex_lock(&gLoupeLock);
    BOOL want = gLoupeWant && !gLoupePending;
    CGPoint at = gLoupeAt;
    CGFloat span = gLoupeSpan;
    if (want) gLoupePending = YES;
    pthread_mutex_unlock(&gLoupeLock);
    if (!want) return;
    CGFloat scale = view.layer.contentsScale;
    GLint vp[4];
    glGetIntegerv(GL_VIEWPORT, vp);
    int n = (int)lround(span * scale);
    int w = vp[2], h = vp[3];
    int x = (int)lround(at.x * scale) - n / 2, y = h - (int)lround(at.y * scale) - n / 2;
    x = MAX(0, MIN(x, w - n));
    y = MAX(0, MIN(y, h - n));
    if (n <= 0 || n > w || n > h)
    {
        pthread_mutex_lock(&gLoupeLock);
        gLoupePending = NO;
        pthread_mutex_unlock(&gLoupeLock);
        return;
    }
    NSMutableData *px = [NSMutableData dataWithLength:(size_t)n * n * 4];
    glReadPixels(x, y, n, n, GL_RGBA, GL_UNSIGNED_BYTE, px.mutableBytes);
    // GL's rows go up: flip
    uint8_t *p = px.mutableBytes, *row = malloc((size_t)n * 4);
    for (int i = 0; i < n / 2; i++)
    {
        memcpy(row, p + (size_t)i * n * 4, (size_t)n * 4);
        memcpy(p + (size_t)i * n * 4, p + (size_t)(n - 1 - i) * n * 4, (size_t)n * 4);
        memcpy(p + (size_t)(n - 1 - i) * n * 4, row, (size_t)n * 4);
    }
    free(row);
    CGDataProviderRef dp = CGDataProviderCreateWithCFData((__bridge CFDataRef)px);
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGImageRef img =
        CGImageCreate(n, n, 8, 32, (size_t)n * 4, cs, kCGImageAlphaNoneSkipLast | kCGBitmapByteOrderDefault, dp, NULL,
                      NO, kCGRenderingIntentDefault);
    CGColorSpaceRelease(cs);
    CGDataProviderRelease(dp);
    dispatch_async(dispatch_get_main_queue(), ^{
        gHost.loupe.layer.contents = (__bridge id)img;
        CGImageRelease(img);
        pthread_mutex_lock(&gLoupeLock);
        gLoupePending = NO;
        pthread_mutex_unlock(&gLoupeLock);
    });
}

static void PresentHook(id self, SEL _cmd)
{
    if (gLoupeWant) LoupeRead(self);
    gPresent(self, _cmd);
}

// Shows the magnifier over a finger at `p` (host points), or hides it (show NO).
static void LoupeShow(BOOL show, CGPoint p)
{
    UIView *loupe = gHost.loupe;
    if (!show)
    {
        pthread_mutex_lock(&gLoupeLock);
        gLoupeWant = NO;
        pthread_mutex_unlock(&gLoupeLock);
        loupe.hidden = YES;
        loupe.layer.contents = nil;
        return;
    }
    CGPoint g = [gHost convertPoint:p toView:gHost.game];
    pthread_mutex_lock(&gLoupeLock);
    gLoupeWant = YES;
    gLoupeAt = g;
    gLoupeSpan = LOUPE_SIZE / (LOUPE_ZOOM * MAX(gHost.zoom, 1));
    pthread_mutex_unlock(&gLoupeLock);
    // Above the finger; below it near the top
    CGFloat y = p.y - LOUPE_SIZE / 2 - 50;
    if (y - LOUPE_SIZE / 2 < 4) y = p.y + LOUPE_SIZE / 2 + 50;
    CGFloat x = MAX(LOUPE_SIZE / 2 + 4, MIN(p.x, gHost.bounds.size.width - LOUPE_SIZE / 2 - 4));
    loupe.center = CGPointMake(x, y);
    if (loupe.hidden)
    {
        loupe.hidden = NO;
        Haptic(UIImpactFeedbackStyleLight);
    }
}

// ---------------------------------------------------------------------------------
// touches: SILView's own methods, wrapped

typedef void (*TouchesFn)(id, SEL, NSSet *, UIEvent *);
static TouchesFn gBegan, gMoved, gEnded, gCancelled;

#define MAXP 10
typedef struct
{
    __unsafe_unretained UITouch *touch;  // UIKit keeps it while it's down
    CGPoint at, start;                   // host points
    CFTimeInterval t0;
    BOOL toGame;                         // the game has it (and it wasn't cancelled)
} Finger;
static Finger gFingers[MAXP];
static enum { M_IDLE, M_SINGLE, M_GESTURE, M_RIGHT } gMode;
static int gGa = -1, gGb = -1;              // the gesture's fingers
static int gRa = -1, gRb = -1;              // right click: the held finger, the pressing one
static CGFloat gD0, gZ0, gGmove;           // pinch: start distance, zoom; most any finger moved
static CGPoint gAnchor;                       // pinch: game point under the fingers (view points)
static CFTimeInterval gGt0;
static BOOL gMinimapPress;
static NSTimer *gLoupeTimer;
static __weak UIView *gGameView;
static UIEvent *gLastEvent;

static int SlotOf(UITouch *t)
{
    for (int i = 0; i < MAXP; i++)
        if (gFingers[i].touch == t) return i;
    return -1;
}
static int FreeSlot(void)
{
    for (int i = 0; i < MAXP; i++)
        if (!gFingers[i].touch) return i;
    return -1;
}
static int ActiveCount(void)
{
    int n = 0;
    for (int i = 0; i < MAXP; i++) n += gFingers[i].touch != nil;
    return n;
}
static CGFloat Dist(Finger *a, Finger *b)
{
    return hypot(a->at.x - b->at.x, a->at.y - b->at.y);
}

static void ToGame(TouchesFn fn, SEL sel, Finger *f)
{
    UIView *v = gGameView;
    if (v && f->touch) fn(v, sel, [NSSet setWithObject:f->touch], gLastEvent);
}
static void CancelForGame(Finger *f)
{
    if (!f->toGame) return;
    ToGame(gCancelled, @selector(touchesCancelled:withEvent:), f);
    f->toGame = NO;
}

static void LoupeStop(void)
{
    [gLoupeTimer invalidate];
    gLoupeTimer = nil;
    LoupeShow(NO, CGPointZero);
}

// A right-button action for Hyperspace at the finger, in the game's pixels
static void RightButton(int action, Finger *f)
{
    UIView *v = gGameView;
    CGPoint p = [gHost convertPoint:f->at toView:v];
    CGFloat k = v.layer.contentsScale;
    HyperspaceRightButton(action, (int)lround(p.x * k), (int)lround(p.y * k));
}

static void GestureBegin(int a, int b)
{
    gMode = M_GESTURE;
    gGa = a;
    gGb = b;
    gD0 = MAX(Dist(&gFingers[a], &gFingers[b]), 1);
    gZ0 = MAX(gHost.zoom, 1);
    CGPoint mid = CGPointMake((gFingers[a].at.x + gFingers[b].at.x) / 2, (gFingers[a].at.y + gFingers[b].at.y) / 2);
    gAnchor = CGPointMake(gHost.x0 + mid.x / gZ0, gHost.y0 + mid.y / gZ0);
    gGt0 = CACurrentMediaTime();
    gGmove = 0;
}

static void GestureUpdate(void)
{
    Finger *a = &gFingers[gGa], *b = &gFingers[gGb];
    CGFloat m =
        MAX(hypot(a->at.x - a->start.x, a->at.y - a->start.y), hypot(b->at.x - b->start.x, b->at.y - b->start.y));
    gGmove = MAX(gGmove, m);
    if (!OptBool(kTouchPinchZoom, YES)) return;  // still a two-finger tap, the view stays
    CGFloat z = MIN(ZOOM_MAX, MAX(1, gZ0 * Dist(a, b) / gD0));
    CGPoint mid = CGPointMake((a->at.x + b->at.x) / 2, (a->at.y + b->at.y) / 2);
    [gHost setZoom:z x0:gAnchor.x - mid.x / z y0:gAnchor.y - mid.y / z animated:0];
}

static void GestureEnd(void)
{
    CFTimeInterval dur = CACurrentMediaTime() - gGt0;
    if (dur < 0.35 && gGmove < 18)
    {  // a two-finger tap
        int paused = OptBool(kTouchTwoFingerPause, YES) ? TogglePause() : -1;
        if (paused >= 0) Haptic(UIImpactFeedbackStyleMedium);
        fprintf(stderr, "[ftlios] two-finger tap: paused %d\n", paused);
    }
    else
    {
        ViewMovedByPlayer();
        if (gHost.zoom < 1.08) ResetZoom(YES);
    }
    gGa = gGb = -1;
}

static void Began(id self, SEL _cmd, NSSet<UITouch *> *touches, UIEvent *event)
{
    gGameView = self;
    gLastEvent = event;
    if (!gHost)
    {
        gBegan(self, _cmd, touches, event);
        return;
    }
    for (UITouch *t in touches)
    {
        int s = FreeSlot();
        if (s < 0) continue;
        CGPoint at = [t locationInView:gHost];
        gFingers[s] = (Finger){t, at, at, CACurrentMediaTime(), NO};
        int n = ActiveCount();
        gFingersDown = YES;
        if (n == 1)
        {
            gMode = M_SINGLE;
            if (!gHost.minimap.hidden && CGRectContainsPoint(gHost.minimap.frame, at))
            {
                gMinimapPress = YES;
                continue;
            }
            gFingers[s].toGame = YES;
            ToGame(gBegan, _cmd, &gFingers[s]);
            if (MagnifierOn())
            {
                CGPoint p = at;
                [gLoupeTimer invalidate];
                gLoupeTimer =
                    [NSTimer scheduledTimerWithTimeInterval:0.15
                                                    repeats:NO
                                                      block:^(NSTimer *x) {
                                                          int i = SlotOf(t);
                                                          if (i >= 0 && gFingers[i].toGame && gMode == M_SINGLE)
                                                              LoupeShow(YES, gFingers[i].at);
                                                          (void)p;
                                                      }];
            }
        }
        else if (gMode == M_RIGHT)
        {
            if (gRb < 0 && gRa >= 0)
            {  // the first finger still down: another right click
                gRb = s;
                RightButton(HS_RIGHT_BUTTON_DOWN, &gFingers[gRa]);
            }
        }
        else if (gMode != M_GESTURE && n == 2)
        {
            int other = -1;
            for (int i = 0; i < MAXP; i++)
                if (gFingers[i].touch && i != s) other = i;
            if (other < 0) continue;
            Finger *o = &gFingers[other];
            LoupeStop();
            gMinimapPress = NO;
            // A finger held still, then a tap: a right click, for mods that listen for one
            if (o->toGame && CACurrentMediaTime() - o->t0 >= 0.25 &&
                hypot(o->at.x - o->start.x, o->at.y - o->start.y) < 18 && HyperspaceWantsRightClick())
            {
                CancelForGame(o);
                gMode = M_RIGHT;
                gRa = other;
                gRb = s;
                RightButton(HS_RIGHT_BUTTON_DOWN, o);
                Haptic(UIImpactFeedbackStyleRigid);
                continue;
            }
            CancelForGame(o);
            GestureBegin(other, s);
        }
    }
}

static void Moved(id self, SEL _cmd, NSSet<UITouch *> *touches, UIEvent *event)
{
    gLastEvent = event;
    if (!gHost)
    {
        gMoved(self, _cmd, touches, event);
        return;
    }
    BOOL gesture = NO;
    for (UITouch *t in touches)
    {
        int s = SlotOf(t);
        if (s < 0) continue;
        gFingers[s].at = [t locationInView:gHost];
        if (gMode == M_RIGHT)
        {
            if (s == gRa && gRb >= 0) RightButton(HS_RIGHT_BUTTON_MOVE, &gFingers[s]);
        }
        else if (gMode == M_GESTURE && (s == gGa || s == gGb))
        {
            gesture = YES;
        }
        else if (gFingers[s].toGame)
        {
            ToGame(gMoved, _cmd, &gFingers[s]);
            if (!gHost.loupe.hidden) LoupeShow(YES, gFingers[s].at);
        }
    }
    if (gesture) GestureUpdate();
}

static void Lifted(id self, SEL _cmd, NSSet<UITouch *> *touches, UIEvent *event, BOOL cancelled)
{
    gLastEvent = event;
    if (!gHost)
    {
        (cancelled ? gCancelled : gEnded)(self, _cmd, touches, event);
        return;
    }
    for (UITouch *t in touches)
    {
        int s = SlotOf(t);
        if (s < 0) continue;
        gFingers[s].at = [t locationInView:gHost];
        if (gMode == M_RIGHT)
        {
            if (gRb >= 0 && (s == gRa || s == gRb))
            {
                RightButton(HS_RIGHT_BUTTON_UP, &gFingers[gRa]);
                gRb = -1;
            }
            if (s == gRa) gRa = -1;
        }
        else
        {
            if (gFingers[s].toGame)
            {
                ToGame(cancelled ? gCancelled : gEnded, _cmd, &gFingers[s]);
                LoupeStop();
            }
            if (gMinimapPress && !cancelled && CGRectContainsPoint(gHost.minimap.frame, gFingers[s].at))
            {
                ViewMovedByPlayer();
                ResetZoom(YES);
            }
            gMinimapPress = NO;
        }
        BOOL gestureFinger = gMode == M_GESTURE && (s == gGa || s == gGb);
        gFingers[s] = (Finger){0};
        if (gestureFinger) GestureEnd();
    }
    if (ActiveCount() == 0)
    {
        gMode = M_IDLE;
        gFingersDown = NO;
    }
}
static void Ended(id self, SEL _cmd, NSSet *touches, UIEvent *event)
{
    Lifted(self, _cmd, touches, event, NO);
}
static void Cancelled(id self, SEL _cmd, NSSet *touches, UIEvent *event)
{
    Lifted(self, _cmd, touches, event, YES);
}

// ---------------------------------------------------------------------------------
// setup

// The game's window got its view: put the view into the host.
void TouchInstall(UIWindow *window)
{
    UIViewController *gameVC = window.rootViewController;
    UIView *game = gameVC.view;
    if (gHost || !game || ![NSStringFromClass(gameVC.class) isEqualToString:@"SILViewController"]) return;
    FTLTouchHostController *vc = [FTLTouchHostController new];
    FTLTouchHost *host = [[FTLTouchHost alloc] initWithFrame:window.bounds];
    host.backgroundColor = UIColor.blackColor;
    host.zoom = 1;
    host.clip = [[UIView alloc] initWithFrame:host.bounds];
    host.clip.clipsToBounds = YES;
    [host addSubview:host.clip];
    vc.view = host;
    window.rootViewController = vc;
    [vc addChildViewController:gameVC];
    host.game = game;
    [host.clip addSubview:game];
    [gameVC didMoveToParentViewController:vc];

    host.loupe = [[UIView alloc] initWithFrame:CGRectMake(0, 0, LOUPE_SIZE, LOUPE_SIZE)];
    host.loupe.layer.cornerRadius = LOUPE_SIZE / 2;
    host.loupe.layer.borderWidth = 2;
    host.loupe.layer.borderColor = UIColor.whiteColor.CGColor;
    host.loupe.layer.masksToBounds = YES;
    host.loupe.layer.magnificationFilter = kCAFilterNearest;
    host.loupe.layer.contentsGravity = kCAGravityResize;
    host.loupe.backgroundColor = UIColor.blackColor;
    host.loupe.userInteractionEnabled = NO;
    host.loupe.hidden = YES;
    [host addSubview:host.loupe];

    host.minimap = [UIView new];
    host.minimap.backgroundColor = [UIColor colorWithWhite:0 alpha:0.45];
    host.minimap.layer.borderColor = [UIColor colorWithWhite:1 alpha:0.7].CGColor;
    host.minimap.layer.borderWidth = 1;
    host.minimap.layer.cornerRadius = 3;
    host.minimap.userInteractionEnabled = NO;
    host.minimap.hidden = YES;
    host.minimapView = [UIView new];
    host.minimapView.backgroundColor = [UIColor colorWithWhite:1 alpha:0.35];
    host.minimapView.layer.borderColor = UIColor.whiteColor.CGColor;
    host.minimapView.layer.borderWidth = 1;
    [host.minimap addSubview:host.minimapView];
    [host addSubview:host.minimap];
    gHost = host;

    [NSTimer scheduledTimerWithTimeInterval:0.1
                                    repeats:YES
                                      block:^(NSTimer *t) {
                                          if (SettingsShowing()) return;
                                          ResumeRunTick();
                                          EventFontTick();
                                          AutozoomTick();
                                      }];
    fprintf(stderr, "[ftlios] touch controls on (magnifier %s)\n", MagnifierOn() ? "on" : "off");
}

__attribute__((constructor)) static void TouchInit(void)
{
    Class view = objc_getClass("SILView");
    if (!view) return;
    struct
    {
        SEL sel;
        IMP imp;
        TouchesFn *orig;
    } wraps[] = {
        {@selector(touchesBegan:withEvent:), (IMP)Began, &gBegan},
        {@selector(touchesMoved:withEvent:), (IMP)Moved, &gMoved},
        {@selector(touchesEnded:withEvent:), (IMP)Ended, &gEnded},
        {@selector(touchesCancelled:withEvent:), (IMP)Cancelled, &gCancelled},
    };
    for (size_t i = 0; i < sizeof wraps / sizeof *wraps; i++)
    {
        Method m = class_getInstanceMethod(view, wraps[i].sel);
        if (!m) return;
        *wraps[i].orig = (TouchesFn)method_setImplementation(m, wraps[i].imp);
    }
    Method present = class_getInstanceMethod(view, @selector(present));
    if (present) gPresent = (void (*)(id, SEL))method_setImplementation(present, (IMP)PresentHook);
}
