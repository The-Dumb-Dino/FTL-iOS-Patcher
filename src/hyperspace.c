// hyperspace.c
//
// Starts Hyperspace (the mod loader Multiverse and other big mods build on), when
// it's installed and the applied mods include it:
//  - Hyperspace.1.6.21.arm64.dylib in the app's Frameworks folder (the patcher's
//    --hyperspace option puts it there; FTLMAC_HYPERSPACE=<path> overrides), and
//  - data/hyperspace.xml in ftl.dat (Hyperspace.ftl applied with ftlman).
// So the same app plays vanilla and other mods untouched. A prepatched game (iOS,
// prepatch.c) always starts it.
//
// It starts before any of the game's code runs, like on ftl-linux, and hooks the
// game in memory. Its logs (zhl.log, FTL_HS.log) and settings_hs.ini go to
// ~/Library/Application Support/FTL for iPad/ (iOS: Documents/Hyperspace). If it can't start, the game
// doesn't either: an alert says why.

#include "hyperspace.h"

#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <errno.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "hooks.h"
#include "hyperspace_host.h"
#include "prepatch.h"

extern const struct mach_header_64 *_NSGetMachExecuteHeader(void);

static bool gRunning;
static hs_android_wants_right_click_fn gWantsRightClick;
static hs_android_right_button_fn gRightButton;

static void HsLog(int level, const char *msg)
{
    fprintf(stderr, "[hyperspace]%s %s\n", level >= 2 ? " error:" : level == 1 ? " warning:" : "", msg);
}

// A blocking alert; works before UIKit is up. CFUserNotification is macOS API
// that Catalyst code can't see in the headers, but it's in CoreFoundation.
static void ShowError(const char *title, const char *message)
{
    HsLog(2, message);
    typedef SInt32 (*AlertFn)(CFTimeInterval, CFOptionFlags, CFURLRef, CFURLRef, CFURLRef, CFStringRef, CFStringRef,
                              CFStringRef, CFStringRef, CFStringRef, CFOptionFlags *);
    AlertFn alert = (AlertFn)dlsym(RTLD_DEFAULT, "CFUserNotificationDisplayAlert");
    if (!alert) return;
    CFStringRef t = CFStringCreateWithCString(NULL, title, kCFStringEncodingUTF8);
    CFStringRef m = CFStringCreateWithCString(NULL, message, kCFStringEncodingUTF8);
    CFOptionFlags response;
    alert(0, 0 /* stop icon */, NULL, NULL, NULL, t, m, CFSTR("Quit"), NULL, NULL, &response);
    CFRelease(t);
    CFRelease(m);
}

// ftl.dat ("PKG\n", big-endian): 16-byte header (magic, header size, entry size,
// entry count, path table size), the entries, then the NUL-separated path table.
static bool DatHas(const char *dat, const char *want)
{
    FILE *f = fopen(dat, "rb");
    if (!f) return false;
    uint8_t h[16];
    bool found = false;
    if (fread(h, 1, 16, f) == 16 && !memcmp(h, "PKG\n\0\x10\0\x14", 8))
    {
        uint32_t n = (uint32_t)h[8] << 24 | h[9] << 16 | h[10] << 8 | h[11];
        uint32_t psize = (uint32_t)h[12] << 24 | h[13] << 16 | h[14] << 8 | h[15];
        char *paths = psize < (64u << 20) ? malloc(psize + 1) : NULL;
        if (paths && !fseek(f, 16 + (long)n * 20, SEEK_SET) && fread(paths, 1, psize, f) == psize)
        {
            paths[psize] = 0;
            for (char *p = paths; p < paths + psize && !found; p += strlen(p) + 1) found = !strcasecmp(p, want);
        }
        free(paths);
    }
    fclose(f);
    return found;
}

// The game's image size, from the start of __TEXT to the end of its last segment.
static size_t ImageSize(const struct mach_header_64 *h)
{
    uint64_t lo = UINT64_MAX, hi = 0;
    const uint8_t *lc = (const uint8_t *)(h + 1);
    for (uint32_t i = 0; i < h->ncmds; i++)
    {
        const struct segment_command_64 *s = (const void *)lc;
        if (s->cmd == LC_SEGMENT_64 && s->vmsize && strcmp(s->segname, "__PAGEZERO"))
        {
            if (s->vmaddr < lo) lo = s->vmaddr;
            if (s->vmaddr + s->vmsize > hi) hi = s->vmaddr + s->vmsize;
        }
        lc += s->cmdsize;
    }
    return hi > lo ? hi - lo : 0;
}

// The app bundle's folder: the executable's.
static bool AppDir(char *out, size_t size)
{
    char exe[PATH_MAX];
    uint32_t n = sizeof exe;
    if (_NSGetExecutablePath(exe, &n) || !realpath(exe, out)) return false;
    char *slash = strrchr(out, '/');
    if (!slash) return false;
    *slash = 0;
    return strlen(out) + 64 < size;
}

void HyperspaceStart(void)
{
    char dir[PATH_MAX], lib[PATH_MAX], dat[PATH_MAX];
    if (!AppDir(dir, sizeof dir)) return;
    const char *env = getenv("FTLMAC_HYPERSPACE");
    snprintf(lib, sizeof lib, "%s", env ? env : "");
    if (!env) snprintf(lib, sizeof lib, "%s/Frameworks/Hyperspace.1.6.21.arm64.dylib", dir);
    snprintf(dat, sizeof dat, "%s/ftl.dat", dir);
    struct stat st;
    if (stat(lib, &st)) return;  // not installed
    // A prepatched game's code already jumps into Hyperspace's trampolines, so it has to start.
    if (PrepatchMode() == PREPATCH_NONE && !DatHas(dat, "data/hyperspace.xml"))
    {
        fprintf(stderr, "[ftlios] Hyperspace is installed, but the applied mods don't include it; not starting it\n");
        return;
    }
    if (!HooksActive())
    {
        ShowError("Hyperspace", "Hyperspace is only for FTL for iPad 1.6.21, and this copy isn't that build.");
        exit(1);
    }

    // Logs and settings: the game's own files live in Application Support too.
    char data[PATH_MAX];
    const char *home = getenv("HOME");
#ifdef FTL_IOS
    // In Documents with the saves, so the logs show in the Files app
    snprintf(data, sizeof data, "%s/Documents/Hyperspace", home ? home : "/tmp");
#else
    snprintf(data, sizeof data, "%s/Library/Application Support/FTL for iPad", home ? home : "/tmp");
#endif
    if (mkdir(data, 0755) && errno != EEXIST) fprintf(stderr, "[ftlios] can't create %s\n", data);

    fprintf(stderr, "[ftlios] starting Hyperspace: %s\n", lib);
    void *h = dlopen(lib, RTLD_NOW | RTLD_LOCAL);
    char msg[1024];
    if (!h)
    {
        snprintf(msg, sizeof msg, "Hyperspace couldn't be loaded: %s", dlerror());
        ShowError("Hyperspace", msg);
        exit(1);
    }
    hs_android_init_fn init = (hs_android_init_fn)dlsym(h, "hs_android_init");
    if (!init)
    {
        ShowError("Hyperspace", "This Hyperspace isn't built for the iPad game (no hs_android_init).");
        exit(1);
    }
    const struct mach_header_64 *mh = _NSGetMachExecuteHeader();
    hs_host_info info = {
        .version = HS_HOST_VERSION,
        .size = sizeof info,
        .mach_header = mh,
        .slide = (intptr_t)mh - 0x100000000,
        .image_size = ImageSize(mh),
        .log_dir = data,
        .data_dir = data,
        .log = HsLog,
        .show_error = ShowError,
        .prepatch_mode = PrepatchMode(),
    };
    if (info.prepatch_mode != PREPATCH_NONE)
        PrepatchHyperspaceAreas(&info.prepatch_code, &info.prepatch_code_size, &info.prepatch_slots,
                                &info.prepatch_slot_count, &info.prepatch_originals, &info.prepatch_originals_size);
    char why[1024] = "";
    if (init(&info, why, sizeof why) != 0)
    {
        snprintf(msg, sizeof msg, "Hyperspace couldn't start: %s\n\nLogs: %s",
                 why[0] ? why : "unknown error (see zhl.log)", data);
        ShowError("Hyperspace", msg);
        exit(1);
    }
    gWantsRightClick = (hs_android_wants_right_click_fn)dlsym(h, "hs_android_wants_right_click");
    gRightButton = (hs_android_right_button_fn)dlsym(h, "hs_android_right_button");
    fprintf(stderr, "[ftlios] Hyperspace started\n");
    gRunning = true;
}

bool HyperspaceRunning(void)
{
    return gRunning;
}

bool HyperspaceWantsRightClick(void)
{
    return gWantsRightClick && gRightButton && gWantsRightClick();
}

void HyperspaceRightButton(int action, int x, int y)
{
    if (gRightButton) gRightButton(action, x, y);
}
