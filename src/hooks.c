// hooks.c
//
// Patches the game's code at startup, before main() runs: keyboard shortcuts,
// the door hotkeys and the game's logger. Also posts key events to the game.
// The addresses (game.h) are for FTL for iPad 1.6.21 only; any other build
// runs without these patches.
//
// Writing code: the game's __TEXT is mapped from the signed file, and
// mprotect() refuses to make it writable (EACCES). vm_protect() with
// VM_PROT_COPY gives the process a private copy of the page instead, which
// can then be made executable again. Trampolines live in anonymous pages
// allocated within branch range (+-128 MB) of the game.

#include "hooks.h"

#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <libkern/OSCacheControl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "display.h"
#include "game.h"
#include "hyperspace.h"
#include "png.h"
#include "prepatch.h"

extern const struct mach_header_64 *_NSGetMachExecuteHeader(void);

static intptr_t gSlide;
static bool gOk;  // the known build, patched
static uintptr_t gImageStart, gImageEnd;

#define GAME(addr) ((void *)((uintptr_t)(addr) + gSlide))

// ---------------------------------------------------------------------------------
// writing code

int CodeWrite(void *dst, const void *src, size_t n)
{
    if (PrepatchMode() == PREPATCH_VERIFY)
    {  // already in the game's file (prepatch.c)
        if (!memcmp(dst, src, n)) return 0;
        fprintf(stderr, "[ftlios] the game's prepatched code at %p doesn't match; patch the game again\n", dst);
        return -1;
    }
    vm_size_t page = vm_page_size;
    vm_address_t start = (uintptr_t)dst & ~(page - 1);
    vm_size_t len = (((uintptr_t)dst + n + page - 1) & ~(page - 1)) - start;
    kern_return_t kr = vm_protect(mach_task_self(), start, len, false, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY);
    if (kr != KERN_SUCCESS)
    {
        fprintf(stderr, "[ftlios] vm_protect(RW) at %p failed: %d\n", dst, kr);
        return -1;
    }
    memcpy(dst, src, n);
    kr = vm_protect(mach_task_self(), start, len, false, VM_PROT_READ | VM_PROT_EXECUTE);
    sys_icache_invalidate(dst, n);
    if (kr != KERN_SUCCESS)
    {
        fprintf(stderr, "[ftlios] vm_protect(RX) at %p failed: %d\n", dst, kr);
        return -1;
    }
    return 0;
}

// Executable memory within branch range of the game: the free gap closest
// to it, found by walking the address space. Returned read + execute; fill it
// with CodeWrite.
void *CodeAlloc(size_t size)
{
    const uint64_t reach = 120ull << 20, align = 0x10000;
    size = (size + align - 1) & ~(align - 1);
    uint64_t lo = gImageStart > reach ? gImageStart - reach : 0, hi = gImageEnd + reach;
    struct
    {
        uint64_t at, dist;
    } cand[64];
    int n = 0;
    vm_address_t at = lo, prevEnd = lo;
    for (;;)
    {
        vm_size_t len = 0;
        vm_region_basic_info_data_64_t info;
        mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t obj;
        bool more = vm_region_64(mach_task_self(), &at, &len, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &count,
                                 &obj) == KERN_SUCCESS;
        uint64_t gapLo = (prevEnd + align - 1) & ~(align - 1), gapHi = more ? at : hi;
        if (gapHi > hi) gapHi = hi;
        if (gapHi > gapLo && gapHi - gapLo >= size && n < 64)
        {
            // the end of the gap next to the game
            uint64_t a = gapLo >= gImageEnd ? gapLo : ((gapHi - size) & ~(align - 1));
            uint64_t d = a >= gImageEnd ? a + size - gImageStart : gImageEnd - a;
            if (a >= gapLo && d < reach) cand[n++] = (typeof(cand[0])){a, d};
        }
        if (!more || at >= hi) break;
        prevEnd = at + len;
        at = prevEnd;
    }
    // closest first; some gaps can't be allocated (the reserved space below the image)
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && cand[j].dist < cand[j - 1].dist; j--)
        {
            typeof(cand[0]) t = cand[j];
            cand[j] = cand[j - 1];
            cand[j - 1] = t;
        }
    for (int i = 0; i < n; i++)
    {
        vm_address_t a = cand[i].at;
        if (vm_allocate(mach_task_self(), &a, size, VM_FLAGS_FIXED) != KERN_SUCCESS) continue;
        if (vm_protect(mach_task_self(), a, size, false, VM_PROT_READ | VM_PROT_EXECUTE) == KERN_SUCCESS)
            return (void *)a;
        vm_deallocate(mach_task_self(), a, size);
    }
    fprintf(stderr, "[ftlios] no executable memory near the game\n");
    return NULL;
}

// A small pool of trampolines.
static uint8_t *gTramp;
static size_t gTrampUsed;

// Replace the instruction at `addr` (unslid) with a branch to `target`,
// through a trampoline that jumps via x17 (like ftl-linux, so Hyperspace,
// which jumps through x16, can hook the same functions later).
static int PatchBranch(uint64_t addr, void *target)
{
    uint32_t *code = GAME(addr);
    if (PrepatchMode() != PREPATCH_NONE)
    {
        // In __HSTEXT, through a pointer in __HSDATA (prepatch.c):
        // adrp x17, slot; ldr x17, [x17, slot & 0xfff]; br x17
        uint8_t *t = PrepatchHostCode(12);
        void **slot = PrepatchHostSlot();
        if (!t || !slot) return -1;
        *slot = target;
        int64_t pages = (int64_t)((uintptr_t)slot >> 12) - (int64_t)((uintptr_t)t >> 12);
        uint32_t jump[3] = {
            0x90000011u | (uint32_t)(pages & 3) << 29 | (uint32_t)((pages >> 2) & 0x7ffff) << 5,
            0xF9400231u | (uint32_t)(((uintptr_t)slot & 0xfff) >> 3) << 10,
            0xD61F0220u,
        };
        if (CodeWrite(t, jump, sizeof jump)) return -1;
        uint32_t b = 0x14000000u | ((uint32_t)(((intptr_t)t - (intptr_t)code) >> 2) & 0x03ffffffu);
        return CodeWrite(code, &b, 4);
    }
    if (!gTramp && !(gTramp = CodeAlloc(vm_page_size))) return -1;
    if (gTrampUsed + 16 > vm_page_size) return -1;
    uint32_t jump[4] = {0x58000051, 0xD61F0220};  // ldr x17, #8; br x17
    memcpy(&jump[2], &target, 8);
    uint8_t *t = gTramp + gTrampUsed;
    if (CodeWrite(t, jump, sizeof jump)) return -1;
    gTrampUsed += 16;
    intptr_t delta = (intptr_t)(t - (uint8_t *)code);
    if (delta >= (1 << 27) || delta < -(1 << 27)) return -1;
    uint32_t b = 0x14000000u | ((uint32_t)(delta >> 2) & 0x03ffffffu);
    return CodeWrite(code, &b, 4);
}

// ---------------------------------------------------------------------------------
// the game's logger

static void GameLog(const char *fmt, ...)
{
    char buf[1024];
    if (!fmt) return;
    va_list ap;
    va_start(ap, fmt);  // the game was built for the same (Apple arm64) variadic ABI
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    size_t n = strlen(buf);
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
    fprintf(stderr, "[FTL] %s\n", buf);
}

// ---------------------------------------------------------------------------------
// keyboard shortcuts: the iOS build keeps the desktop hotkey handling but its
// binding lookup always answers 0. Answer with the desktop game's defaults
// (SDL 1.2 keysyms), as ftl-linux does.

enum
{
    K_SPACE = 32,
    K_F1 = 282
};
static const struct
{
    const char *action;
    int key;
} HOTKEYS[] = {
    {"pause", K_SPACE},
    {"jump", 'j'},
    {"ship_info", 'u'},
    {"ship_crew", 'c'},
    {"ship_inv", 'i'},
    {"autofire", 'v'},
    {"open", 'z'},
    {"close", 'x'},
    {"send_tele", 't'},
    {"ret_tele", 'r'},
    {"lockdown", 'l'},
    {"savePositions", '['},
    {"loadPositions", ']'},
    {"crew_all", '`'},
    // systems (Shift + key removes power)
    {"shields", 'a'},
    {"engines", 's'},
    {"oxygen", 'f'},
    {"medbay", 'd'},
    {"clonebay", 'd'},
    {"weapons", 'w'},
    {"drones", 'e'},
    {"teleporter", 'g'},
    {"cloaking", 'h'},
    {"artillery", 'y'},
    {"battery", 'b'},
    {"mind", 'n'},
    {"hacking", 'm'},
    // weapon slots then drone slots (Shift + key removes power)
    {"power_1", '1'},
    {"power_2", '2'},
    {"power_3", '3'},
    {"power_4", '4'},
    {"power_5", '5'},
    {"power_6", '6'},
    {"power_7", '7'},
    {"power_8", '8'},
    {"crew1", K_F1},
    {"crew2", K_F1 + 1},
    {"crew3", K_F1 + 2},
    {"crew4", K_F1 + 3},
    {"crew5", K_F1 + 4},
    {"crew6", K_F1 + 5},
    {"crew7", K_F1 + 6},
    {"crew8", K_F1 + 7},
    // Hyperspace's command console, for mods that turn it on (Multiverse does)
    {"console", '\\'},
};

// libc++ std::string (Apple's alternate layout)
static const char *CxxString(const void *s, size_t *len)
{
    const uint8_t *b = s;
    if (b[23] & 0x80)
    {
        *len = *(const size_t *)(b + 8);
        return *(const char *const *)b;
    }
    *len = b[23];
    return (const char *)b;
}

static bool gDebug;  // FTLMAC_DEBUG: log keys and hotkey lookups

static int HotkeyLookup(const void *action)
{
    if (!action) return 0;
    size_t n;
    const char *a = CxxString(action, &n);
    int key = 0;
    for (size_t i = 0; i < sizeof HOTKEYS / sizeof *HOTKEYS && !key; i++)
        if (strlen(HOTKEYS[i].action) == n && !memcmp(HOTKEYS[i].action, a, n)) key = HOTKEYS[i].key;
    if (gDebug) fprintf(stderr, "[ftlios] hotkey '%.*s' -> %d\n", (int)n, a, key);
    return key;
}

// ---------------------------------------------------------------------------------
// setup

static const uint8_t KNOWN_UUID[16] = {0x87, 0x3d, 0x2b, 0x34, 0xd7, 0x07, 0x3d, 0x6b,
                                       0xab, 0x83, 0x26, 0x7b, 0xf4, 0xc5, 0xbb, 0xc0};

// Checks the build, sets the slide and the image's end.
static bool KnownBuild(const struct mach_header_64 *h)
{
    bool uuidOk = false;
    uint64_t textVmaddr = 0, end = 0;
    const uint8_t *lc = (const uint8_t *)(h + 1);
    for (uint32_t i = 0; i < h->ncmds; i++)
    {
        const struct load_command *c = (const void *)lc;
        if (c->cmd == LC_UUID) uuidOk = !memcmp(((const struct uuid_command *)c)->uuid, KNOWN_UUID, 16);
        if (c->cmd == LC_SEGMENT_64)
        {
            const struct segment_command_64 *s = (const void *)c;
            if (!strcmp(s->segname, "__TEXT")) textVmaddr = s->vmaddr;
            if (s->vmaddr + s->vmsize > end) end = s->vmaddr + s->vmsize;
        }
        lc += c->cmdsize;
    }
    gSlide = (intptr_t)h - (intptr_t)textVmaddr;
    gImageStart = (uintptr_t)h;
    gImageEnd = (uintptr_t)(end + gSlide);
    return uuidOk && textVmaddr == 0x100000000ull;
}

// Whether the code at addr is what this build has there (`n` words). A prepatched
// game already has the changes in place: CodeWrite compares them instead.
static bool Expect(uint64_t addr, const uint32_t *words, size_t n)
{
    return PrepatchMode() == PREPATCH_VERIFY || !memcmp(GAME(addr), words, n * 4);
}
#define EXPECT(addr, ...) Expect(addr, (const uint32_t[]){__VA_ARGS__}, sizeof((uint32_t[]){__VA_ARGS__}) / 4)

static void Apply(void)
{
    gDebug = getenv("FTLMAC_DEBUG") != NULL;
    const struct mach_header_64 *h = _NSGetMachExecuteHeader();
    if (!h || h->magic != MH_MAGIC_64 || !KnownBuild(h))
    {
        fprintf(stderr, "[ftlios] not the FTL 1.6.21 iPad build; keyboard and logger hooks disabled\n");
        return;
    }
    PrepatchInit(gSlide);
    int fails = 0;
    if (EXPECT(G_LOG_FN, 0xD65F03C0u))  // ret
        fails += PatchBranch(G_LOG_FN, (void *)GameLog) != 0;
    else
        fprintf(stderr, "[ftlios] unexpected code at the game's logger; not hooking it\n");
    if (EXPECT(G_HOTKEY, 0x52800000u, 0xD65F03C0u))  // mov w0, #0; ret
        fails += PatchBranch(G_HOTKEY, (void *)HotkeyLookup) != 0;
    else
        fprintf(stderr, "[ftlios] unexpected code at the hotkey lookup; keyboard shortcuts disabled\n");
    // The open/close-all-doors keys are tied to the door control buttons, which
    // the iPad build only shows in door mode. Accept them any time, like the
    // desktop game; the ship still checks that the door system is working.
    if (EXPECT(G_DOOR_KEY_OPEN, 0x34000228u) && EXPECT(G_DOOR_KEY_CLOSE, 0x34000228u))
    {  // cbz w8, ...
        uint32_t nop = 0xD503201Fu;
        fails += CodeWrite(GAME(G_DOOR_KEY_OPEN), &nop, 4) != 0;
        fails += CodeWrite(GAME(G_DOOR_KEY_CLOSE), &nop, 4) != 0;
    }
    else
    {
        fprintf(stderr, "[ftlios] unexpected code in the door hotkeys; Z/X only work in door mode\n");
    }
    // The iOS platform layer reports "no hardware keyboard" every frame, and
    // while it does the game ignores held keys, so Shift + hotkey (remove
    // power) never works. Report one: same instructions, reordered so the
    // flag gets w8's low byte (1) instead of wzr.
    uint32_t *caps = GAME(G_INPUT_CAPS);
    static const uint32_t capsIos[4] = {
        0x3900427fu,  // strb wzr, [x19, #0x10]  keyboard: no
        0x39004a7fu,  // strb wzr, [x19, #0x12]
        0x3200c3e8u,  // mov w8, #0x1010101
        0xb8013268u,  // stur w8, [x19, #0x13]
    };
    static const uint32_t capsMac[4] = {
        0x3200c3e8u,  // mov w8, #0x1010101
        0x39004268u,  // strb w8, [x19, #0x10]   keyboard: yes
        0x39004a7fu,  // strb wzr, [x19, #0x12]
        0xb8013268u,  // stur w8, [x19, #0x13]
    };
    if (Expect(G_INPUT_CAPS, capsIos, 4))
        fails += CodeWrite(caps, capsMac, sizeof capsMac) != 0;
    else
        fprintf(stderr, "[ftlios] unexpected code in the input caps; Shift + hotkey won't remove power\n");
#ifdef FTL_IOS
    // Saves and settings in Documents instead of Library/Application Support, so they
    // show in the Files app (On My iPhone > FTL) and can be copied in and out.
    if (EXPECT(G_SAVE_FOLDER, 0x528001C0u))
    {  // mov w0, #14 (NSApplicationSupportDirectory)
        uint32_t documents = 0x52800120u;       // mov w0, #9 (NSDocumentDirectory)
        fails += CodeWrite(GAME(G_SAVE_FOLDER), &documents, 4) != 0;
    }
    else
    {
        fprintf(stderr, "[ftlios] unexpected code at the save folder lookup; saves stay in Application Support\n");
    }
#endif
    // PNG images from mods (png.c)
    if (EXPECT(G_PNG_PARSE, 0xD2800000u, 0xD65F03C0u))  // mov x0, #0; ret
        fails += PatchBranch(G_PNG_PARSE, (void *)PngParse) != 0;
    else
        fprintf(stderr, "[ftlios] unexpected code at png_parse; PNG images from mods won't load\n");
    gOk = fails == 0;
    fprintf(stderr, "[ftlios] FTL 1.6.21, slide %#lx: hooks %s\n", (long)gSlide, gOk ? "applied" : "FAILED");
}

// Runs before the game's main(), while only this thread exists. Hyperspace hooks
// the game after these patches (some of the same functions), as on ftl-linux.
void IosStartLog(void) __attribute__((weak));  // ios.m: the session log (iOS)

__attribute__((constructor)) static void HooksInit(void)
{
    if (IosStartLog) IosStartLog();
    Apply();
    DisplayInstall();
    HyperspaceStart();
    PrepatchRecordFinish();  // record mode (prepatch.c): writes the changes and ends here
}

bool HooksActive(void)
{
    return gOk;
}

void *GameAddr(uint64_t addr)
{
    return GAME(addr);
}

// ---------------------------------------------------------------------------------
// game state and events (main thread)

typedef struct
{
    int32_t cls, type;
    double time;
    int32_t i0;  // key code
    float f1, f2, f3, f4;
    int32_t i5;
} SilEvent;
_Static_assert(sizeof(SilEvent) == 40, "sil_event");
enum
{
    SIL_CLS_KEY = 2,
    SIL_KEY_DOWN = 7,
    SIL_KEY_UP = 8
};

// The game's event clock: mach_absolute_time in seconds.
static double NowSeconds(void)
{
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return (double)mach_absolute_time() * tb.numer / tb.denom * 1e-9;
}

void GameKey(int silCode, bool down)
{
    if (!gOk || silCode <= 0) return;
    if (gDebug) fprintf(stderr, "[ftlios] key %d %s\n", silCode, down ? "down" : "up");
    SilEvent e = {SIL_CLS_KEY, down ? SIL_KEY_DOWN : SIL_KEY_UP, 0, silCode, 0, 0, 0, 0, 0};
    double base = *(double *)GAME(G_TIME_BASE);
    e.time = base > 0 ? NowSeconds() - base : 0;
    ((void (*)(SilEvent *))GAME(G_POST_EVENT))(&e);
}

double GameTimeBaseDebug(double *now)
{
    *now = NowSeconds();
    return gOk ? *(double *)GAME(G_TIME_BASE) : -1;
}
