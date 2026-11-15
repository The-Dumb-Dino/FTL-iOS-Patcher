// iOS only: see ios.m (scenes, saves, ftl.dat, logs) and settings.m (the settings screen).
#pragma once
#import <UIKit/UIKit.h>

#define IOS_SETTINGS_SHORTCUT @"ftl.settings"  // the home screen quick action (Info.plist)

// ios.m
NSString *IosDocumentsDir(void);     // the game's saves and settings
NSString *IosLogsDir(void);          // session.log, session.1.log, session.2.log
NSString *IosGameDataDir(void);      // the copy of ftl.dat: Documents/Game Data
const char *IosGameDataFile(void);   // the copy the game reads; NULL when it reads the app's own
void IosPrepareGameData(void);       // hooks.c: the copy exists, is up to date and fixed up
void IosMakeFilesUsable(void);       // hooks.c: the app can read and write its own files
void IosResetGameData(void);         // settings: back to the app's own at the next start
BOOL IosIsGameFile(NSString *name);  // in the documents folder: a save, settings, players/
void IosGameActive(BOOL active);     // the game pauses while it isn't active
UIWindowScene *IosScene(void);       // the game's

// touch.m: touch controls; their options (NSUserDefaults keys)
void TouchInstall(UIWindow *window);  // the game's window is up
extern NSString *const kTouchMagnifier,
    *const kTouchTwoFingerPause,
        *const kTouchPinchZoom, *const kTouchMinimap, *const kTouchWindowZoom, *const kTouchEventZoom,
                                                                                   *const kTouchEventText,
                                                                                       *const kTouchResumeRun,
                                                                                           *const kTouchHaptics,
                                                                                               *const kTouchPortrait;
BOOL TouchIsIphoneDuo(void);
BOOL TouchPortraitAllowed(void);
void TouchOrientationChanged(void);  // kTouchPortrait changed

// settings.m
void SettingsShow(void);
BOOL SettingsShowing(void);
