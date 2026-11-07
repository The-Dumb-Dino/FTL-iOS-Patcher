// settings.m
//
// iOS only: the settings screen, opened from the app icon's Settings quick action
// (ios.m). The game pauses while it's open.
//  - Saves: export them as a zip (share sheet: Files, AirDrop, ...), import a zip
//    made that way or single .sav / settings.ini files, and undo the last import.
//    The game only reads its saves when it starts, and iOS apps can't restart
//    themselves, so FTL closes after an import.
//  - Game Center: on or off (the game's own setting; it asks for it when Game
//    Center doesn't answer, but has no page in the Settings app for it).
//  - Troubleshooting: the last sessions' logs (ios.m) and Hyperspace's, as a zip.
//  - About: versions, and whether Hyperspace runs.

#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <zlib.h>

#import "ios.h"

#pragma clang diagnostic ignored "-Wobjc-string-concatenation"  // the footers are long
#include "hyperspace.h"

static NSString *const kGameCenter = @"EnableGameCenter";  // the game's NSUserDefaults key

// ---------------------------------------------------------------------------------
// files

static NSString *BackupDir(void)
{  // the saves an import replaced
    NSString *lib = NSSearchPathForDirectoriesInDomains(NSLibraryDirectory, NSUserDomainMask, YES).firstObject;
    return [lib stringByAppendingPathComponent:@"Saves before import"];
}

// Where a file from an export or a pick goes, relative to Documents; nil if it isn't
// one of the game's files. Folders an export or an archiver added in front are dropped.
static NSString *SaveDestination(NSString *path)
{
    NSArray<NSString *> *parts = path.pathComponents;
    for (NSUInteger i = 0; i < parts.count; i++)
    {
        NSString *p = parts[i];
        BOOL last = i + 1 == parts.count;
        if (last && [p isEqualToString:@"settings_hs.ini"]) return @"Hyperspace/settings_hs.ini";
        if (last && IosIsGameFile(p)) return p;
        if ([p isEqualToString:@"players"] && i + 2 < parts.count && IosIsGameFile(parts.lastObject))
            return [NSString pathWithComponents:[parts subarrayWithRange:NSMakeRange(i, parts.count - i)]];
    }
    return nil;
}

// The game's files in Documents, relative to it.
static NSArray<NSString *> *currentSaves(void)
{
    NSFileManager *fm = NSFileManager.defaultManager;
    NSString *docs = IosDocumentsDir();
    NSMutableArray *out = [NSMutableArray array];
    for (NSString *rel in [fm subpathsOfDirectoryAtPath:docs error:nil])
    {
        BOOL dir = NO;
        [fm fileExistsAtPath:[docs stringByAppendingPathComponent:rel] isDirectory:&dir];
        if (!dir && [SaveDestination(rel) isEqualToString:rel]) [out addObject:rel];
    }
    return out;
}

static uint32_t Le32(const uint8_t *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint16_t Le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

// The files in a zip (stored or deflated), by path. nil if it isn't a zip.
static NSDictionary<NSString *, NSData *> *unzip(NSData *zip)
{
    const uint8_t *p = zip.bytes;
    size_t n = zip.length;
    if (n < 22) return nil;
    size_t eocd = SIZE_MAX;
    for (size_t i = n - 22 + 1; i-- > 0 && n - i < 22 + 65536;)
        if (Le32(p + i) == 0x06054b50)
        {
            eocd = i;
            break;
        }
    if (eocd == SIZE_MAX) return nil;
    NSMutableDictionary *files = [NSMutableDictionary dictionary];
    size_t at = Le32(p + eocd + 16);
    for (unsigned count = Le16(p + eocd + 10); count > 0; count--)
    {
        if (at + 46 > n || Le32(p + at) != 0x02014b50) return nil;
        uint16_t method = Le16(p + at + 10), nlen = Le16(p + at + 28);
        uint32_t csize = Le32(p + at + 20), usize = Le32(p + at + 24), local = Le32(p + at + 42);
        size_t next = at + 46 + nlen + Le16(p + at + 30) + Le16(p + at + 32);
        if (next > n) return nil;
        NSString *name = [[NSString alloc] initWithBytes:p + at + 46 length:nlen encoding:NSUTF8StringEncoding];
        at = next;
        if (!name || [name hasSuffix:@"/"] || usize > (64u << 20)) continue;
        if ((size_t)local + 30 > n || Le32(p + local) != 0x04034b50) return nil;
        size_t data = local + 30 + Le16(p + local + 26) + Le16(p + local + 28);
        if (data + csize > n) return nil;
        NSMutableData *out = [NSMutableData dataWithLength:usize];
        if (method == 0 && csize == usize)
        {
            memcpy(out.mutableBytes, p + data, usize);
        }
        else if (method == 8)
        {
            z_stream z = {0};
            if (inflateInit2(&z, -MAX_WBITS) != Z_OK) return nil;
            z.next_in = (Bytef *)(p + data);
            z.avail_in = csize;
            z.next_out = out.mutableBytes;
            z.avail_out = usize;
            int r = inflate(&z, Z_FINISH);
            inflateEnd(&z);
            if (r != Z_STREAM_END) continue;
        }
        else
        {
            continue;
        }
        files[name] = out;
    }
    return files;
}

// A zip of these files (paths relative to `base`), named `name`.zip, in the temporary folder.
static NSURL *MakeZip(NSString *name, NSString *base, NSArray<NSString *> *paths, NSError **error)
{
    NSFileManager *fm = NSFileManager.defaultManager;
    NSString *tmp = [NSTemporaryDirectory() stringByAppendingPathComponent:NSUUID.UUID.UUIDString];
    NSString *folder = [tmp stringByAppendingPathComponent:name];
    for (NSString *rel in paths)
    {
        NSString *to = [folder stringByAppendingPathComponent:rel];
        [fm createDirectoryAtPath:to.stringByDeletingLastPathComponent
            withIntermediateDirectories:YES
                             attributes:nil
                                  error:nil];
        if (![fm copyItemAtPath:[base stringByAppendingPathComponent:rel] toPath:to error:error]) return nil;
    }
    // Reading a folder "for uploading" gives a zip of it.
    __block NSURL *zip = nil;
    __block NSError *copyError = nil;
    NSError *coordError = nil;
    [[NSFileCoordinator new]
        coordinateReadingItemAtURL:[NSURL fileURLWithPath:folder]
                           options:NSFileCoordinatorReadingForUploading
                             error:&coordError
                        byAccessor:^(NSURL *url) {
                            NSURL *out = [NSURL fileURLWithPath:[tmp stringByAppendingPathComponent:
                                                                         [name stringByAppendingPathExtension:@"zip"]]];
                            if ([fm copyItemAtURL:url toURL:out error:&copyError]) zip = out;
                        }];
    if (error) *error = coordError ?: copyError;
    return zip;
}

// ---------------------------------------------------------------------------------
// the screen

// A row: @"title", and @"detail" (shown on the right), @"action" (a selector name: tapped),
// @"flag" with @"default" (a switch for that NSUserDefaults key) or @"key" with
// @"choices" (picks one; the key holds its index).
typedef NSDictionary<NSString *, id> *Row;

@interface FTLSettingsController : UITableViewController <UIDocumentPickerDelegate>
@property(nonatomic, weak) UIView *anchor;  // the row last tapped
@end

// The settings window's root: the same orientations as the game.
@interface FTLSettingsRoot : UIViewController
@end
@implementation FTLSettingsRoot
- (UIInterfaceOrientationMask)supportedInterfaceOrientations
{
    return TouchPortraitAllowed() ? UIInterfaceOrientationMaskAll : UIInterfaceOrientationMaskLandscape;
}
@end

static BOOL gShowing;
// Settings get their own window: the game's, made before scenes, reports the
// insets of portrait while it's in landscape, so the Dynamic Island would cover them.
static UIWindow *gWindow, *gGameWindow;

static void SettingsClosed(void)
{
    gWindow.hidden = YES;
    gWindow = nil;
    [gGameWindow makeKeyWindow];
    gShowing = NO;
    IosGameActive(YES);
}

@implementation FTLSettingsController
{
    NSArray<NSString *> *_sections, *_footers;
    NSArray<NSArray<Row> *> *_rows;
}

- (instancetype)init
{
    self = [super initWithStyle:UITableViewStyleInsetGrouped];
    self.title = @"FTL Settings";
    self.navigationItem.rightBarButtonItem =
        [[UIBarButtonItem alloc] initWithBarButtonSystemItem:UIBarButtonSystemItemDone
                                                      target:self
                                                      action:@selector(done)];
    [self reload];
    return self;
}

static Row RowItem(NSString *title, NSString *detail, SEL action)
{
    NSMutableDictionary *r = [NSMutableDictionary dictionaryWithObject:title forKey:@"title"];
    if (detail) r[@"detail"] = detail;
    if (action) r[@"action"] = NSStringFromSelector(action);
    return r;
}
static Row FlagItem(NSString *title, NSString *key, BOOL dflt)
{
    return @{@"title" : title, @"flag" : key, @"default" : @(dflt)};
}
static Row ChoiceItem(NSString *title, NSString *key, NSArray<NSString *> *choices)
{
    return @{@"title" : title, @"key" : key, @"choices" : choices};
}

- (void)reload
{
    BOOL backup = [NSFileManager.defaultManager fileExistsAtPath:BackupDir()];
    NSDictionary *info = NSBundle.mainBundle.infoDictionary;
    NSArray *autoOnOff = @[ @"Automatic (phones)", @"Always", @"Off" ];
    _sections = @[ @"Touch", @"Zoom", @"Playing", @"Saves", @"Game Center", @"Troubleshooting", @"About" ];
    _rows = @[
        @[
            ChoiceItem(@"Magnifier", kTouchMagnifier, autoOnOff),
            FlagItem(@"Two-Finger Tap Pauses", kTouchTwoFingerPause, YES),
            FlagItem(@"Vibration", kTouchHaptics, YES),
            FlagItem(@"Portrait Orientation", kTouchPortrait, TouchIsIphoneDuo()),
        ],
        @[
            FlagItem(@"Pinch to Zoom", kTouchPinchZoom, YES),
            FlagItem(@"Minimap While Zoomed", kTouchMinimap, YES),
            ChoiceItem(@"Zoom In on Map and Upgrades", kTouchWindowZoom, autoOnOff),
            FlagItem(@"Zoom In on Events", kTouchEventZoom, NO),
        ],
        @[
            ChoiceItem(@"Event Text", kTouchEventText, @[ @"Normal (as on the iPad)", @"Large", @"Larger" ]),
            FlagItem(@"Continue the Run at Launch", kTouchResumeRun, NO),
        ],
        @[
            RowItem(@"Export Saves…", nil, @selector(exportSaves)),
            RowItem(@"Import Saves…", nil, @selector(importSaves)),
            RowItem(@"Restore Earlier Saves", backup ? @"From before the last import" : nil,
                    backup ? @selector(restoreSaves) : NULL),
        ],
        @[ FlagItem(@"Game Center", kGameCenter, YES) ],
        @[ RowItem(@"Save Logs…", nil, @selector(exportLogs)) ],
        @[
            RowItem(@"FTL", info[@"CFBundleShortVersionString"], NULL),
            RowItem(@"Hyperspace", HyperspaceRunning() ? @"Running" : @"Not installed", NULL),
        ],
    ];
    _footers = @[
        @"Hold a finger to see a magnified view of what's under it; slide onto a small button, then lift. "
        @"Two fingers tapped together pause and resume. A finger held and a tap with another is a right click, "
        @"for mods that use one. Portrait orientation lets the game turn upright too, with bars above and below "
        @"(on by default on the iPhone Duo).",
        @"Pinch to zoom, drag with two fingers to move around. The minimap shows where you are; tap it to zoom "
        @"back out. The beacon map and the upgrade screen can fill the screen while they're open; events too, "
        @"which hides the rest of the screen.",
        @"Larger event text makes the event window bigger too. Continuing the run at launch skips the title screen "
        @"when there's a run saved.",
        [NSString stringWithFormat:@"Your run, profile (unlocks, achievements, scores) and settings are also in the Files "
                                   @"app: On My %@ › %@. An import replaces the saves it contains; FTL closes afterwards "
                                   @"so it can load them.", UIDevice.currentDevice.model,
                                   info[@"CFBundleDisplayName"] ?: info[@"CFBundleName"] ?: @"FTL"],
        @"Takes effect the next time FTL starts.",
        @"The logs of the last three times FTL ran, and Hyperspace's. They help find out what went wrong.",
        @"",
    ];
    [self.tableView reloadData];
}

- (Row)rowAt:(NSIndexPath *)ip
{
    return _rows[ip.section][ip.row];
}

- (NSInteger)numberOfSectionsInTableView:(UITableView *)tv
{
    return _sections.count;
}
- (NSInteger)tableView:(UITableView *)tv numberOfRowsInSection:(NSInteger)s
{
    return _rows[s].count;
}
- (NSString *)tableView:(UITableView *)tv titleForHeaderInSection:(NSInteger)s
{
    return _sections[s];
}
- (NSString *)tableView:(UITableView *)tv titleForFooterInSection:(NSInteger)s
{
    return _footers[s].length ? _footers[s] : nil;
}

- (UITableViewCell *)tableView:(UITableView *)tv cellForRowAtIndexPath:(NSIndexPath *)ip
{
    Row r = [self rowAt:ip];
    UITableViewCell *cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleValue1 reuseIdentifier:nil];
    cell.textLabel.text = r[@"title"];
    cell.detailTextLabel.text = r[@"detail"];
    if (r[@"flag"])
    {
        UISwitch *sw = [UISwitch new];
        id v = [NSUserDefaults.standardUserDefaults objectForKey:r[@"flag"]];
        sw.on = v ? [v boolValue] : [r[@"default"] boolValue];
        sw.accessibilityIdentifier = r[@"flag"];
        [sw addTarget:self action:@selector(flagChanged:) forControlEvents:UIControlEventValueChanged];
        cell.accessoryView = sw;
        cell.selectionStyle = UITableViewCellSelectionStyleNone;
    }
    else if (r[@"choices"])
    {
        NSArray *choices = r[@"choices"];
        NSInteger i = [NSUserDefaults.standardUserDefaults integerForKey:r[@"key"]];
        cell.detailTextLabel.text = choices[i >= 0 && i < (NSInteger)choices.count ? i : 0];
        cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
    }
    else if (r[@"action"])
    {
        cell.textLabel.textColor = self.view.tintColor;
    }
    else
    {
        cell.selectionStyle = UITableViewCellSelectionStyleNone;
        if (!r[@"detail"]) cell.textLabel.textColor = UIColor.tertiaryLabelColor;  // not available
    }
    return cell;
}

- (void)tableView:(UITableView *)tv didSelectRowAtIndexPath:(NSIndexPath *)ip
{
    [tv deselectRowAtIndexPath:ip animated:YES];
    Row r = [self rowAt:ip];
    if (r[@"action"])
    {
        self.anchor = [tv cellForRowAtIndexPath:ip];  // where the share sheet points on iPad
        SEL sel = NSSelectorFromString(r[@"action"]);
        ((void (*)(id, SEL))[self methodForSelector:sel])(self, sel);
    }
    else if (r[@"choices"])
    {
        UIAlertController *a = [UIAlertController alertControllerWithTitle:r[@"title"]
                                                                   message:nil
                                                            preferredStyle:UIAlertControllerStyleActionSheet];
        NSArray *choices = r[@"choices"];
        for (NSUInteger i = 0; i < choices.count; i++)
            [a addAction:[UIAlertAction actionWithTitle:choices[i]
                                                  style:UIAlertActionStyleDefault
                                                handler:^(UIAlertAction *x) {
                                                    [NSUserDefaults.standardUserDefaults setInteger:(NSInteger)i
                                                                                             forKey:r[@"key"]];
                                                    [self reload];
                                                }]];
        [a addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
        UITableViewCell *cell = [tv cellForRowAtIndexPath:ip];
        a.popoverPresentationController.sourceView = cell;  // iPad
        a.popoverPresentationController.sourceRect = cell.bounds;
        [self presentViewController:a animated:YES completion:nil];
    }
}

- (void)flagChanged:(UISwitch *)sw
{
    [NSUserDefaults.standardUserDefaults setBool:sw.on forKey:sw.accessibilityIdentifier];
    if ([sw.accessibilityIdentifier isEqualToString:kTouchPortrait]) TouchOrientationChanged();
}

- (void)done
{
    [self dismissViewControllerAnimated:YES
                             completion:^{
                                 SettingsClosed();
                             }];
}

- (void)alert:(NSString *)title message:(NSString *)message
{
    UIAlertController *a = [UIAlertController alertControllerWithTitle:title
                                                               message:message
                                                        preferredStyle:UIAlertControllerStyleAlert];
    [a addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
    [self presentViewController:a animated:YES completion:nil];
}

- (void)share:(NSURL *)url
{
    UIActivityViewController *vc = [[UIActivityViewController alloc] initWithActivityItems:@[ url ]
                                                                     applicationActivities:nil];
    // iPad: a popover at the row that was tapped. Not at a point in self.view, the table:
    // that's in the scrolled content, so once scrolled down the popover was off screen,
    // invisible, and kept anything else from being presented.
    UIView *anchor = self.anchor.window ? self.anchor : self.navigationController.view;
    vc.popoverPresentationController.sourceView = anchor;
    vc.popoverPresentationController.sourceRect =
        anchor == self.anchor ? anchor.bounds
                              : CGRectMake(anchor.bounds.size.width / 2, anchor.bounds.size.height / 2, 1, 1);
    [self presentViewController:vc animated:YES completion:nil];
}

// The game reads its saves only when it starts.
- (void)closeFTL:(NSString *)message
{
    UIAlertController *a = [UIAlertController alertControllerWithTitle:@"FTL Closes Now"
                                                               message:message
                                                        preferredStyle:UIAlertControllerStyleAlert];
    [a addAction:[UIAlertAction actionWithTitle:@"Close FTL"
                                          style:UIAlertActionStyleDefault
                                        handler:^(UIAlertAction *x) {
                                            fprintf(stderr, "[ftlios] closing after a change of saves\n");
                                            _exit(0);  // without the game saving over them on the way out
                                        }]];
    [self presentViewController:a animated:YES completion:nil];
}

// ---- saves

- (void)exportSaves
{
    NSArray *saves = currentSaves();
    if (!saves.count)
    {
        [self alert:@"No Saves Yet" message:@"Play a little first: FTL saves your profile and run as you go."];
        return;
    }
    NSDateFormatter *f = [NSDateFormatter new];
    f.dateFormat = @"yyyy-MM-dd HH.mm";
    NSError *err = nil;
    NSURL *zip = MakeZip([NSString stringWithFormat:@"FTL saves %@", [f stringFromDate:NSDate.date]], IosDocumentsDir(),
                         saves, &err);
    if (!zip)
    {
        [self alert:@"Couldn't Export Saves" message:err.localizedDescription];
        return;
    }
    [self share:zip];
}

- (void)importSaves
{
    UIDocumentPickerViewController *picker =
        [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[ UTTypeZIP, UTTypeData, UTTypePlainText ]
                                                                    asCopy:YES];
    picker.allowsMultipleSelection = YES;
    picker.delegate = self;
    [self presentViewController:picker animated:YES completion:nil];
}

- (void)documentPicker:(UIDocumentPickerViewController *)picker didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls
{
    NSMutableDictionary<NSString *, NSData *> *found = [NSMutableDictionary dictionary];
    for (NSURL *url in urls)
    {
        NSData *data = [NSData dataWithContentsOfURL:url];
        if (!data) continue;
        NSDictionary *files = unzip(data);
        if (files)
        {
            [files enumerateKeysAndObjectsUsingBlock:^(NSString *path, NSData *d, BOOL *stop) {
                NSString *to = SaveDestination(path);
                if (to && d.length) found[to] = d;
            }];
        }
        else
        {
            NSString *to = SaveDestination(url.lastPathComponent);
            if (to && data.length) found[to] = data;
        }
    }
    if (!found.count)
    {
        [self alert:@"No FTL Saves Found"
            message:@"Pick a zip made with Export Saves, or the files continue.sav, prof.sav, ae_prof.sav or "
                    @"settings.ini."];
        return;
    }
    NSArray *names = [found.allKeys sortedArrayUsingSelector:@selector(compare:)];
    UIAlertController *a = [UIAlertController
        alertControllerWithTitle:@"Replace Your Saves?"
                         message:[NSString
                                     stringWithFormat:@"These replace the matching files you have now:\n\n%@\n\n"
                                                      @"Your current ones are kept, so Restore Earlier Saves can put "
                                                      @"them back. FTL closes afterwards.",
                                                      [names componentsJoinedByString:@"\n"]]
                  preferredStyle:UIAlertControllerStyleAlert];
    [a addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [a addAction:[UIAlertAction actionWithTitle:@"Replace"
                                          style:UIAlertActionStyleDestructive
                                        handler:^(UIAlertAction *x) {
                                            [self replaceSaves:found];
                                        }]];
    [self presentViewController:a animated:YES completion:nil];
}

- (void)replaceSaves:(NSDictionary<NSString *, NSData *> *)files
{
    NSFileManager *fm = NSFileManager.defaultManager;
    NSString *docs = IosDocumentsDir(), *backup = BackupDir();
    [fm removeItemAtPath:backup error:nil];  // only the last import's
    [fm createDirectoryAtPath:backup withIntermediateDirectories:YES attributes:nil error:nil];
    NSMutableArray *written = [NSMutableArray array];
    for (NSString *rel in files)
    {
        NSString *to = [docs stringByAppendingPathComponent:rel];
        if ([fm fileExistsAtPath:to])
        {
            NSString *keep = [backup stringByAppendingPathComponent:rel];
            [fm createDirectoryAtPath:keep.stringByDeletingLastPathComponent
                withIntermediateDirectories:YES
                                 attributes:nil
                                      error:nil];
            [fm moveItemAtPath:to toPath:keep error:nil];
        }
        [fm createDirectoryAtPath:to.stringByDeletingLastPathComponent
            withIntermediateDirectories:YES
                             attributes:nil
                                  error:nil];
        if ([files[rel] writeToFile:to atomically:YES]) [written addObject:rel];
    }
    // What was new rather than replaced is listed too, so restoring removes it.
    [[written componentsJoinedByString:@"\n"] writeToFile:[backup stringByAppendingPathComponent:@".imported"]
                                               atomically:YES
                                                 encoding:NSUTF8StringEncoding
                                                    error:nil];
    fprintf(stderr, "[ftlios] imported saves: %s\n", [written componentsJoinedByString:@", "].UTF8String);
    [self closeFTL:@"Open FTL again from the Home Screen to play with the imported saves."];
}

- (void)restoreSaves
{
    UIAlertController *a = [UIAlertController
        alertControllerWithTitle:@"Restore Earlier Saves?"
                         message:@"This puts back the saves you had before the last import. FTL closes afterwards."
                  preferredStyle:UIAlertControllerStyleAlert];
    [a addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [a addAction:[UIAlertAction
                     actionWithTitle:@"Restore"
                               style:UIAlertActionStyleDestructive
                             handler:^(UIAlertAction *x) {
                                 NSFileManager *fm = NSFileManager.defaultManager;
                                 NSString *docs = IosDocumentsDir(), *backup = BackupDir();
                                 NSString *list = [NSString
                                     stringWithContentsOfFile:[backup stringByAppendingPathComponent:@".imported"]
                                                     encoding:NSUTF8StringEncoding
                                                        error:nil];
                                 for (NSString *rel in [list componentsSeparatedByString:@"\n"])
                                     if (rel.length)
                                         [fm removeItemAtPath:[docs stringByAppendingPathComponent:rel] error:nil];
                                 for (NSString *rel in [fm subpathsOfDirectoryAtPath:backup error:nil])
                                 {
                                     BOOL dir = NO;
                                     NSString *from = [backup stringByAppendingPathComponent:rel];
                                     if ([rel isEqualToString:@".imported"] ||
                                         ([fm fileExistsAtPath:from isDirectory:&dir] && dir))
                                         continue;
                                     NSString *to = [docs stringByAppendingPathComponent:rel];
                                     [fm createDirectoryAtPath:to.stringByDeletingLastPathComponent
                                         withIntermediateDirectories:YES
                                                          attributes:nil
                                                               error:nil];
                                     [fm moveItemAtPath:from toPath:to error:nil];
                                 }
                                 [fm removeItemAtPath:backup error:nil];
                                 fprintf(stderr, "[ftlios] restored the saves from before the import\n");
                                 [self
                                     closeFTL:@"Open FTL again from the Home Screen to play with your earlier saves."];
                             }]];
    [self presentViewController:a animated:YES completion:nil];
}

// ---- logs

- (void)exportLogs
{
    fflush(stderr);
    NSFileManager *fm = NSFileManager.defaultManager;
    NSString *tmp = [NSTemporaryDirectory() stringByAppendingPathComponent:NSUUID.UUID.UUIDString];
    NSString *logs = IosLogsDir(), *hs = [IosDocumentsDir() stringByAppendingPathComponent:@"Hyperspace"];
    [fm createDirectoryAtPath:tmp withIntermediateDirectories:YES attributes:nil error:nil];
    NSMutableArray *paths = [NSMutableArray array];
    for (NSString *dir in @[ logs, hs ])
        for (NSString *name in [fm contentsOfDirectoryAtPath:dir error:nil])
            if ([name.pathExtension isEqualToString:@"log"] || [name hasSuffix:@".log.bak"])
            {
                NSString *rel = [dir isEqualToString:hs] ? [@"Hyperspace" stringByAppendingPathComponent:name] : name;
                [fm createDirectoryAtPath:[tmp stringByAppendingPathComponent:rel].stringByDeletingLastPathComponent
                    withIntermediateDirectories:YES
                                     attributes:nil
                                          error:nil];
                [fm copyItemAtPath:[dir stringByAppendingPathComponent:name]
                            toPath:[tmp stringByAppendingPathComponent:rel]
                             error:nil];
                [paths addObject:rel];
            }
    NSError *err = nil;
    NSURL *zip = paths.count ? MakeZip(@"FTL logs", tmp, paths, &err) : nil;
    if (!zip)
    {
        [self alert:@"Couldn't Save Logs" message:err.localizedDescription ?: @"There are no logs yet."];
        return;
    }
    [self share:zip];
}
@end

void SettingsShow(void)
{
    if (gShowing) return;
    UIWindowScene *scene = IosScene();
    if (!scene)
    {
        fprintf(stderr, "[ftlios] settings: no scene to show them in\n");
        return;
    }
    gShowing = YES;
    IosGameActive(NO);  // pauses the run and the music
    for (UIWindow *w in scene.windows)
        if (w.isKeyWindow) gGameWindow = w;
    gWindow = [[UIWindow alloc] initWithWindowScene:scene];
    gWindow.rootViewController = [FTLSettingsRoot new];
    gWindow.windowLevel = UIWindowLevelNormal + 1;
    [gWindow makeKeyAndVisible];
    UINavigationController *nav =
        [[UINavigationController alloc] initWithRootViewController:[FTLSettingsController new]];
    nav.modalPresentationStyle = UIModalPresentationFormSheet;
    nav.modalInPresentation = YES;  // closed with Done, so the game resumes
    [gWindow.rootViewController presentViewController:nav animated:YES completion:nil];
    fprintf(stderr, "[ftlios] settings: shown\n");
}

BOOL SettingsShowing(void)
{
    return gShowing;
}
