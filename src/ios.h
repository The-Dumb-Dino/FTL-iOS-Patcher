// iOS only: see ios.m (scenes, saves, logs) and settings.m (the settings screen).
#pragma once
#import <UIKit/UIKit.h>

#define IOS_SETTINGS_SHORTCUT @"ftl.settings"  // the home screen quick action (Info.plist)

// ios.m
NSString *IosDocumentsDir(void);         // the game's saves and settings
NSString *IosLogsDir(void);              // session.log, session.1.log, session.2.log
BOOL IosIsGameFile(NSString *name);     // in the documents folder: a save, settings, players/
void IosGameActive(BOOL active);         // the game pauses while it isn't active
UIWindowScene *IosScene(void);            // the game's

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
