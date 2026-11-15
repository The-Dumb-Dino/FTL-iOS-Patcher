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
//  - ftl.dat and logs. The game reads its data file from Documents/Game Data, not from
//    the app: a copy made from the bundle at the first start, which the Files app and
//    other apps can replace (a mod manager, say). The iPad mod fix-up (gamedata.c) is
//    applied to it before the game opens it. Everything written to stderr (the game's
//    log, ours, Hyperspace's) also goes to Documents/Logs/session.log; the two sessions
//    before are kept as session.1.log and session.2.log. Settings (settings.m) exports
//    the logs.
//  - Settings. Long-pressing the app's icon offers Settings (a home screen quick
//    action), which opens settings.m's screen over the game.
//
// Everything here goes through the Objective-C runtime: no code is patched, so a
// prepatched game needs nothing for it.

#import <UIKit/UIKit.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <sys/clonefile.h>
#include <sys/stat.h>
#include <unistd.h>

#import "gamedata.h"
#import "image.h"
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
// the game's folders in Documents

NSString *IosDocumentsDir(void)
{
    return NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
}

// Not in iCloud backups: the copy of ftl.dat is large and easy to make again, and the
// logs are only useful for a while.
static void ExcludeFromBackup(NSString *path)
{
    [[NSURL fileURLWithPath:path] setResourceValue:@YES forKey:NSURLIsExcludedFromBackupKey error:nil];
}

// ---------------------------------------------------------------------------------
// the game's data file

// Documents/Game Data: a folder of its own, so the Files app lists it next to the saves.
static NSString *GameDataDir(void)
{
    return [IosDocumentsDir() stringByAppendingPathComponent:@"Game Data"];
}

static NSString *GameDataPath(NSString *name)
{
    return [GameDataDir() stringByAppendingPathComponent:name];
}

// The app's own ftl.dat: in the game's bundle. The game's image knows where it was
// loaded from; a host (LiveContainer) runs the game as a guest, and its main bundle is
// the host's, not the game's.
static NSString *GameBundlePath(void)
{
    const char *image = GameImagePath();
    if (image) return [[NSString stringWithUTF8String:image] stringByDeletingLastPathComponent];
    return NSBundle.mainBundle.bundlePath;
}

// A file's size and modification time, in milliseconds: whether it changed.
static NSDictionary *StatOf(NSString *path)
{
    NSDictionary *a = [NSFileManager.defaultManager attributesOfItemAtPath:path error:nil];
    if (!a) return nil;
    return @{
        @"size" : @([a[NSFileSize] unsignedLongLongValue]),
        @"time" : @(llround([a[NSFileModificationDate] timeIntervalSince1970] * 1000))
    };
}

static BOOL StatMatches(NSDictionary *stat, NSDictionary *state, NSString *sizeKey, NSString *timeKey)
{
    return stat && state && [state[sizeKey] isEqual:stat[@"size"]] && [state[timeKey] isEqual:stat[@"time"]];
}

// A clone when the file system can (instant; the copy shares the bundle's blocks), else
// a plain copy.
static BOOL CopyGameData(NSString *from, NSString *to)
{
    NSFileManager *fm = NSFileManager.defaultManager;
    [fm removeItemAtPath:to error:nil];
    if (!clonefile(from.fileSystemRepresentation, to.fileSystemRepresentation, 0))
    {
        chmod(to.fileSystemRepresentation, 0644);
        return YES;
    }
    NSError *err = nil;
    if ([fm copyItemAtPath:from toPath:to error:&err]) return YES;
    IosLog(@"couldn't copy ftl.dat: %@", err.localizedDescription);
    return NO;
}

// The game asks its bundle for resourcePath once at startup and opens ftl.dat there.
// That's what it uses it for (it also checks for a DesignLibrary bundle, which the app
// doesn't have either), so pointing it at the copy is enough.
static BOOL gGameDataReady;
static IMP gResourcePath;

// Bump when the fix-up changes (gamedata.c), so copies that were fixed up by an older
// version are done again.
static const NSInteger kGameDataVersion = 1;

static NSString *GameResourcePath(id self, SEL _cmd)
{
    NSString *path = ((NSString * (*)(id, SEL)) gResourcePath)(self, _cmd);
    if (gGameDataReady && self == NSBundle.mainBundle) return GameDataDir();
    return path;
}

static void InstallResourcePath(void)
{
    static BOOL installed;
    if (installed) return;
    installed = YES;
    Method m = class_getInstanceMethod(NSBundle.class, @selector(resourcePath));
    if (!m) return;
    gResourcePath = method_setImplementation(m, (IMP)GameResourcePath);
    IosLog(@"the game reads its data from %@", GameDataDir());
}

// Before the game opens anything: the copy exists and is up to date, and the mods'
// images in it are fixed up (gamedata.c). Another app may have replaced it since the
// last start, so it's fixed up again then.
void IosPrepareGameData(void)
{
    @autoreleasepool
    {
        NSFileManager *fm = NSFileManager.defaultManager;
        NSString *from = [GameBundlePath() stringByAppendingPathComponent:@"ftl.dat"];
        if (![fm fileExistsAtPath:from])
        {
            IosLog(@"no ftl.dat in the game's bundle");
            return;
        }
        NSString *dir = GameDataDir(), *to = GameDataPath(@"ftl.dat"), *statePath = GameDataPath(@".state.plist");
        [fm createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
        ExcludeFromBackup(dir);

        NSDictionary *bundleStat = StatOf(from), *copyStat = StatOf(to);
        NSDictionary *state = [NSDictionary dictionaryWithContentsOfFile:statePath];
        // A copy that hasn't been changed since the last start was fixed up then.
        BOOL untouched =
            [state[@"version"] isEqual:@(kGameDataVersion)] && StatMatches(copyStat, state, @"copySize", @"copyTime");
        BOOL bundleChanged = !StatMatches(bundleStat, state, @"bundleSize", @"bundleTime");
        BOOL copied = NO;
        // An update changed the app's own ftl.dat: make the copy again, unless it was
        // changed in the meantime (then it's the player's, or another app's).
        if (!copyStat || (untouched && bundleChanged))
        {
            IosLog(@"%@ ftl.dat in Documents", copyStat ? @"replacing the copy of" : @"copying");
            if (!CopyGameData(from, to)) return;
            copied = YES;
        }
        int fixed = GAME_DATA_UNCHANGED;
        chmod(to.fileSystemRepresentation, 0644);  // another app may have made the copy read-only
        if (copied || !untouched) fixed = GameDataFix(to.fileSystemRepresentation);
        if (fixed == GAME_DATA_CHANGED) IosLog(@"fixed up the mods' images in ftl.dat");
        if (fixed == GAME_DATA_ERROR)
        {
            IosLog(@"ftl.dat in Documents isn't readable; the game uses the app's own");
            return;
        }

        // What the next start compares against. The fix-up rewrites the index, so the
        // copy's size and time are taken afterwards.
        copyStat = StatOf(to);
        if (bundleStat && copyStat)
            [@{
                @"version" : @(kGameDataVersion),
                @"bundleSize" : bundleStat[@"size"],
                @"bundleTime" : bundleStat[@"time"],
                @"copySize" : copyStat[@"size"],
                @"copyTime" : copyStat[@"time"]
            } writeToFile:statePath
                atomically:YES];
        gGameDataReady = YES;
        InstallResourcePath();
    }
}

// Settings: away with the copy; the app's own is copied again at the next start.
void IosResetGameData(void)
{
    @autoreleasepool
    {
        NSFileManager *fm = NSFileManager.defaultManager;
        [fm removeItemAtPath:GameDataPath(@"ftl.dat") error:nil];
        [fm removeItemAtPath:GameDataPath(@".state.plist") error:nil];
        gGameDataReady = NO;  // the rest of this run uses the app's own
        IosLog(@"removed the copy of ftl.dat");
    }
}

NSString *IosGameDataDir(void)
{
    return GameDataDir();
}

// Saves and settings can come from elsewhere (another app in Files, a restore) with
// permissions that keep the app from reading them. Hyperspace's save hook takes a save it
// can't open for an empty FILE and crashes the game on so much as a pause, so the app's
// own files are made readable and writable again before the game starts.
void IosMakeFilesUsable(void)
{
    @autoreleasepool
    {
        NSFileManager *fm = NSFileManager.defaultManager;
        NSString *docs = IosDocumentsDir();
        int fixed = 0;
        for (NSString *rel in [fm subpathsOfDirectoryAtPath:docs error:nil])
        {
            NSString *path = [docs stringByAppendingPathComponent:rel];
            NSDictionary *a = [fm attributesOfItemAtPath:path error:nil];
            BOOL dir = [a[NSFileType] isEqual:NSFileTypeDirectory];
            mode_t mode = a[NSFilePosixPermissions] ? [a[NSFilePosixPermissions] unsignedShortValue] : 0;
            mode_t want = mode | (dir ? 0700 : 0600);  // through it, and read and write it
            if (want != mode && !chmod(path.fileSystemRepresentation, want)) fixed++;
        }
        if (fixed) IosLog(@"made %d of the game's files usable again", fixed);
    }
}

// For C (hyperspace.c): where the copy is; NULL when the game reads the app's own.
const char *IosGameDataFile(void)
{
    static char path[PATH_MAX];
    if (!gGameDataReady) return NULL;
    if (![GameDataPath(@"ftl.dat") getFileSystemRepresentation:path maxLength:sizeof path]) return NULL;
    return path;
}

// ---------------------------------------------------------------------------------
// session logs

NSString *IosLogsDir(void)
{
    return [IosDocumentsDir() stringByAppendingPathComponent:@"Logs"];
}

// Logs used to be in Library/Logs: move the last ones over once, so the Files app has
// them right away.
static void MoveOldLogs(NSString *to)
{
    NSFileManager *fm = NSFileManager.defaultManager;
    NSString *lib = NSSearchPathForDirectoriesInDomains(NSLibraryDirectory, NSUserDomainMask, YES).firstObject;
    NSString *old = [lib stringByAppendingPathComponent:@"Logs"];
    for (NSString *name in @[ @"session.log", @"session.1.log", @"session.2.log" ])
    {
        NSString *from = [old stringByAppendingPathComponent:name];
        NSString *dst = [to stringByAppendingPathComponent:name];
        if ([fm fileExistsAtPath:from] && ![fm fileExistsAtPath:dst]) [fm moveItemAtPath:from toPath:dst error:nil];
    }
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
        ExcludeFromBackup(dir);
        MoveOldLogs(dir);
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

// The app delegate's callbacks, as the scene delegate below passes them on.
static void ForwardToGame(SEL sel)
{
    if (gAppDelegate && [gAppDelegate respondsToSelector:sel])
        ((void (*)(id, SEL, UIApplication *))objc_msgSend)(gAppDelegate, sel, UIApplication.sharedApplication);
}

// A host that runs the game as a guest (LiveContainer) uses its own Info.plist: its scenes
// have no delegate of ours, so the app-level active and inactive callbacks go to nobody.
// Pass the notifications on instead.
static void InstallActiveNotifications(void)
{
    static BOOL installed;
    if (installed) return;
    installed = YES;
    NSNotificationCenter *nc = NSNotificationCenter.defaultCenter;
    [nc addObserverForName:UIApplicationDidBecomeActiveNotification
                    object:nil
                     queue:nil
                usingBlock:^(NSNotification *note) {
                    if (!SettingsShowing()) ForwardToGame(@selector(applicationDidBecomeActive:));
                }];
    [nc addObserverForName:UIApplicationWillResignActiveNotification
                    object:nil
                     queue:nil
                usingBlock:^(NSNotification *note) {
                    if (!SettingsShowing()) ForwardToGame(@selector(applicationWillResignActive:));
                }];
}

static BOOL gWaitedForLandscape;

static BOOL SceneIsPortrait(void)
{
    CGSize s = gScene.coordinateSpace.bounds.size;
    return s.height > s.width;
}

// FTL's own launch code, once there's something to show it in.
static void StartGame(void)
{
    if (gLaunched || !gAppDelegate) return;
    gLaunched = YES;
    ((BOOL (*)(id, SEL, UIApplication *, NSDictionary *))gFinishLaunching)(
        gAppDelegate, @selector(application:didFinishLaunchingWithOptions:), UIApplication.sharedApplication,
        gLaunchOptions);
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
    // A scene of a host's (below): the app-level callbacks don't come any more
    if (![gScene.delegate isKindOfClass:FTLSceneDelegate.class]) InstallActiveNotifications();
    StartGame();
}

// The process's scene, when it isn't ours to delegate: a host that runs the game as a
// guest (LiveContainer) uses its own Info.plist, so our scene delegate is never made.
// The active one, when there are several (a host that gives each app its own).
static UIWindowScene *ConnectedScene(void)
{
    UIWindowScene *first = nil;
    for (id s in UIApplication.sharedApplication.connectedScenes)
    {
        if (![s isKindOfClass:UIWindowScene.class]) continue;
        if ([(UIWindowScene *)s activationState] == UISceneActivationStateForegroundActive) return (UIWindowScene *)s;
        if (!first) first = (UIWindowScene *)s;
    }
    return first;
}

// No scene ever came: take the one the host has, or start the game the old way, without.
static void LaunchWithoutScene(void)
{
    if (gLaunched || !gAppDelegate || gScene) return;
    if ((gScene = ConnectedScene())) return LaunchGame();
    InstallActiveNotifications();
    IosLog(@"no scene; starting the game without one");
    StartGame();
}

// FTL's launch code runs once there's a scene to show it in.
static BOOL DeferredLaunch(id self, SEL _cmd, UIApplication *app, NSDictionary *options)
{
    gAppDelegate = self;
    gLaunchOptions = options;
    if (!gScene) gScene = ConnectedScene();
    LaunchGame();
    if (!gLaunched)
    {
        // The scene hasn't connected yet; normally it does at once
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 1 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
            LaunchWithoutScene();
        });
    }
    return YES;
}

// FTL makes its window the old way, without a scene: put it on ours.
static void MakeKeyAndVisible(UIWindow *self, SEL _cmd)
{
    if (!self.windowScene && gScene)
    {
        self.windowScene = gScene;
        self.frame = gScene.coordinateSpace.bounds;
        if ([gScene.delegate isKindOfClass:FTLSceneDelegate.class]) ((FTLSceneDelegate *)gScene.delegate).window = self;
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
    ForwardToGame(sel);
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
    ForwardToGame(active ? @selector(applicationDidBecomeActive:) : @selector(applicationWillResignActive:));
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
