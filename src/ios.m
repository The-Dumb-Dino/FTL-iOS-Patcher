// ios.m
//
// iOS only (ftlios.dylib):
//  - Scenes. The patcher stamps the game as built with a current iOS SDK, so new
//    iPhones show it fullscreen instead of letterboxed; apps built with it must use
//    the UIScene lifecycle, which FTL predates. FTLSceneDelegate starts FTL's own
//    launch code once its scene exists, puts FTL's window on that scene and passes
//    the scene's pause and resume on to FTL's app delegate. (The same approach as the
//    FTL iPhone Duo patch's FTLSceneShim.)
//  - Saves. hooks.c makes the game keep its saves and settings in Documents, which
//    the Files app shows (On My iPhone > FTL). Saves from before that, in Library/
//    Application Support, move there once.
//  - Logs. Everything written to stderr (the game's log, ours, Hyperspace's) also goes
//    to Library/Logs/session.log; the two sessions before are kept as session.1.log
//    and session.2.log. Settings (settings.m) exports them.
//  - Settings. Long-pressing the app's icon offers Settings (a home screen quick
//    action), which opens settings.m's screen over the game.
//
// Everything here goes through the Objective-C runtime: no code is patched, so a
// prepatched game needs nothing for it.

#import <UIKit/UIKit.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <pthread.h>
#include <unistd.h>

#import "ios.h"

static void IosLog(NSString *fmt, ...) NS_FORMAT_FUNCTION(1, 2);
static void IosLog(NSString *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    NSString *s = [[NSString alloc] initWithFormat:fmt arguments:ap];
    va_end(ap);
    fprintf(stderr, "[ftlios] %s\n", s.UTF8String);
}

// ---------------------------------------------------------------------------------
// saves

// ---------------------------------------------------------------------------------
// session logs

NSString *IosLogsDir(void)
{
    NSString *lib = NSSearchPathForDirectoriesInDomains(NSLibraryDirectory, NSUserDomainMask, YES).firstObject;
    return [lib stringByAppendingPathComponent:@"Logs"];
}

static int gConsole = -1;  // stderr as it was: the debugger's or the simulator's console

// Copies what's written to stderr into the log file and on to the console.
static void *LogTee(void *arg)
{
    int in = (int)(intptr_t)arg;
    FILE *file = fopen([IosLogsDir() stringByAppendingPathComponent:@"session.log"].fileSystemRepresentation, "w");
    char buf[4096];
    ssize_t n;
    while ((n = read(in, buf, sizeof buf)) > 0)
    {
        if (gConsole >= 0) write(gConsole, buf, (size_t)n);
        if (file)
        {
            fwrite(buf, 1, (size_t)n, file);
            fflush(file);
        }
    }
    return NULL;
}

// Called first thing by hooks.c's constructor (the linker doesn't order constructors),
// so the log has the patches and Hyperspace too.
void IosStartLog(void)
{
    static BOOL started;
    if (started) return;
    started = YES;
    @autoreleasepool
    {
        NSFileManager *fm = NSFileManager.defaultManager;
        NSString *dir = IosLogsDir();
        [fm createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
        for (int i = 2; i > 0; i--)
        {
            NSString *to = [dir stringByAppendingPathComponent:[NSString stringWithFormat:@"session.%d.log", i]];
            NSString *from =
                [dir stringByAppendingPathComponent:i == 1 ? @"session.log"
                                                           : [NSString stringWithFormat:@"session.%d.log", i - 1]];
            [fm removeItemAtPath:to error:nil];
            [fm moveItemAtPath:from toPath:to error:nil];
        }
        int fds[2];
        if (pipe(fds)) return;
        gConsole = dup(STDERR_FILENO);
        dup2(fds[1], STDERR_FILENO);
        close(fds[1]);
        setvbuf(stderr, NULL, _IOLBF, 0);
        pthread_t t;
        pthread_create(&t, NULL, LogTee, (void *)(intptr_t)fds[0]);
        pthread_detach(t);
        NSDictionary *info = NSBundle.mainBundle.infoDictionary;
        fprintf(stderr, "[ftlios] FTL %s, %s %s, %s\n", [info[@"CFBundleShortVersionString"] UTF8String],
                UIDevice.currentDevice.systemName.UTF8String, UIDevice.currentDevice.systemVersion.UTF8String,
                [NSDate.date.description UTF8String]);
    }
}

NSString *IosDocumentsDir(void)
{
    return NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
}

// The game's files: its settings, its saves (*.sav) and, with Game Center, a folder
// per player.
BOOL IosIsGameFile(NSString *name)
{
    return [name isEqualToString:@"settings.ini"] || [name isEqualToString:@"settings_hs.ini"] ||
           [name isEqualToString:@"players"] || [name.pathExtension isEqualToString:@"sav"];
}

static void MoveOldSaves(void)
{
    NSFileManager *fm = NSFileManager.defaultManager;
    NSString *docs = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
    NSString *old =
        NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES).firstObject;
    if (!docs || !old) return;
    for (NSString *name in [fm contentsOfDirectoryAtPath:docs error:nil])
        if (IosIsGameFile(name)) return;  // already playing from Documents
    for (NSString *name in [fm contentsOfDirectoryAtPath:old error:nil])
    {
        if (!IosIsGameFile(name)) continue;
        NSError *err = nil;
        if ([fm moveItemAtPath:[old stringByAppendingPathComponent:name]
                        toPath:[docs stringByAppendingPathComponent:name]
                         error:&err])
            IosLog(@"moved %@ to Documents", name);
        else
            IosLog(@"couldn't move %@ to Documents: %@", name, err.localizedDescription);
    }
}

// ---------------------------------------------------------------------------------
// scenes

static IMP gFinishLaunching;  // SILAppDelegate's application:didFinishLaunchingWithOptions:
static id gAppDelegate;
static NSDictionary *gLaunchOptions;
static UIWindowScene *gScene;
static BOOL gLaunched;
static IMP gMakeKeyAndVisible;
static BOOL gShowSettings;  // launched from the Settings quick action

@interface FTLSceneDelegate : UIResponder <UIWindowSceneDelegate>
@property(nonatomic, strong) UIWindow *window;
@end

static BOOL gWaitedForLandscape;

static BOOL SceneIsPortrait(void)
{
    CGSize s = gScene.coordinateSpace.bounds.size;
    return s.height > s.width;
}

static void LaunchGame(void)
{
    if (gLaunched || !gScene || !gAppDelegate) return;
    // The game sizes its canvas once, at launch. iPadOS 26 and newer start an app held
    // upright in portrait even when it only supports landscape, so unless portrait is
    // allowed, ask for landscape and launch once the scene has turned (or after a while).
    if (!TouchPortraitAllowed() && SceneIsPortrait() && !gWaitedForLandscape)
    {
        gWaitedForLandscape = YES;
        if (@available(iOS 16.0, *))
        {
            UIWindowSceneGeometryPreferencesIOS *prefs = [[UIWindowSceneGeometryPreferencesIOS alloc]
                initWithInterfaceOrientations:UIInterfaceOrientationMaskLandscape];
            [gScene requestGeometryUpdateWithPreferences:prefs
                                            errorHandler:^(NSError *error) {
                                                IosLog(@"landscape request: %@", error.localizedDescription);
                                            }];
        }
        IosLog(@"scene is portrait, waiting for landscape");
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
            LaunchGame();
        });
        return;
    }
    gLaunched = YES;
    ((BOOL (*)(id, SEL, UIApplication *, NSDictionary *))gFinishLaunching)(
        gAppDelegate, @selector(application:didFinishLaunchingWithOptions:), UIApplication.sharedApplication,
        gLaunchOptions);
}

// FTL's launch code runs once there's a scene to show it in.
static BOOL DeferredLaunch(id self, SEL _cmd, UIApplication *app, NSDictionary *options)
{
    gAppDelegate = self;
    gLaunchOptions = options;
    LaunchGame();
    return YES;
}

// FTL makes its window the old way, without a scene: put it on ours.
static void MakeKeyAndVisible(UIWindow *self, SEL _cmd)
{
    if (!self.windowScene && gScene)
    {
        self.windowScene = gScene;
        self.frame = gScene.coordinateSpace.bounds;
        ((FTLSceneDelegate *)gScene.delegate).window = self;
    }
    ((void (*)(id, SEL))gMakeKeyAndVisible)(self, _cmd);
    if ([NSStringFromClass(self.rootViewController.class) isEqualToString:@"SILViewController"])
        dispatch_async(dispatch_get_main_queue(), ^{
            TouchInstall(self);
        });
}

@implementation FTLSceneDelegate
- (void)scene:(UIScene *)scene
    willConnectToSession:(UISceneSession *)session
                 options:(UISceneConnectionOptions *)options
{
    if (![scene isKindOfClass:UIWindowScene.class] || gScene) return;
    gScene = (UIWindowScene *)scene;
    CGRect b = gScene.coordinateSpace.bounds;
    IosLog(@"scene %.0fx%.0f points", b.size.width, b.size.height);
    gShowSettings = [options.shortcutItem.type isEqualToString:IOS_SETTINGS_SHORTCUT];
    LaunchGame();
}
- (void)windowScene:(UIWindowScene *)scene
    didUpdateCoordinateSpace:(id<UICoordinateSpace>)previous
        interfaceOrientation:(UIInterfaceOrientation)orientation
             traitCollection:(UITraitCollection *)traits
{
    if (!gLaunched && gWaitedForLandscape && !SceneIsPortrait())
    {
        CGRect b = scene.coordinateSpace.bounds;
        IosLog(@"scene %.0fx%.0f points", b.size.width, b.size.height);
        LaunchGame();
    }
}
- (void)windowScene:(UIWindowScene *)scene
    performActionForShortcutItem:(UIApplicationShortcutItem *)item
               completionHandler:(void (^)(BOOL))done
{
    // Comes while the scene isn't active yet: shown once it is (sceneDidBecomeActive).
    BOOL settings = [item.type isEqualToString:IOS_SETTINGS_SHORTCUT];
    if (settings) gShowSettings = YES;
    done(settings);
}
- (void)forward:(SEL)sel
{
    if ([gAppDelegate respondsToSelector:sel])
        ((void (*)(id, SEL, UIApplication *))objc_msgSend)(gAppDelegate, sel, UIApplication.sharedApplication);
}
- (void)sceneDidBecomeActive:(UIScene *)scene
{
    if (!SettingsShowing()) [self forward:@selector(applicationDidBecomeActive:)];
    if (gShowSettings)
    {
        gShowSettings = NO;
        // On the next pass of the run loop, once the game's view is back
        dispatch_async(dispatch_get_main_queue(), ^{
            SettingsShow();
        });
    }
}
- (void)sceneWillResignActive:(UIScene *)scene
{
    if (!SettingsShowing()) [self forward:@selector(applicationWillResignActive:)];
}
@end

// The game pauses (its run, its music) while it's not active; settings use that.
void IosGameActive(BOOL active)
{
    SEL sel = active ? @selector(applicationDidBecomeActive:) : @selector(applicationWillResignActive:);
    if ([gAppDelegate respondsToSelector:sel])
        ((void (*)(id, SEL, UIApplication *))objc_msgSend)(gAppDelegate, sel, UIApplication.sharedApplication);
}

UIWindowScene *IosScene(void)
{
    return gScene;
}

static void InstallScenes(void)
{
    // Only when the patcher declared the scene (Info.plist), else the old lifecycle runs.
    NSDictionary *manifest = NSBundle.mainBundle.infoDictionary[@"UIApplicationSceneManifest"];
    if (!manifest) return;
    Class delegate = objc_getClass("SILAppDelegate");
    Method launch =
        delegate ? class_getInstanceMethod(delegate, @selector(application:didFinishLaunchingWithOptions:)) : NULL;
    Method show = class_getInstanceMethod(UIWindow.class, @selector(makeKeyAndVisible));
    if (!launch || !show)
    {
        IosLog(@"no SILAppDelegate launch method; scenes not set up");
        return;
    }
    gFinishLaunching = method_setImplementation(launch, (IMP)DeferredLaunch);
    gMakeKeyAndVisible = method_setImplementation(show, (IMP)MakeKeyAndVisible);
}

__attribute__((constructor)) static void IosInit(void)
{
    @autoreleasepool
    {
        MoveOldSaves();
        InstallScenes();
    }
}
