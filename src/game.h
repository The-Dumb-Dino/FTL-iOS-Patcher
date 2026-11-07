// Addresses and struct offsets in FTL for iPad 1.6.21 (arm64 slice, LC_UUID
// 873d2b34-d707-3d6b-ab83-267bf4c5bbc0). Unslid vmaddrs (base 0x100000000).
// Taken from ftl-linux (native/src/ux/game.h). hooks.c checks the UUID first.
#pragma once

// ---- functions ----
#define G_LOG_FN         0x10006051cull      // printf-style logger, compiled to `ret`
#define G_POST_EVENT     0x100117268ull  // void post_event(SilEvent *)
#define G_TIME_BASE      0x100250c88ull   // double, event timestamp base (s)
#define G_HOTKEY         0x100040728ull      // int (const std::string *action): `return 0` on iOS
// DoorBox::KeyDown: `cbz` that skips the open / close hotkey while that button is hidden
#define G_DOOR_KEY_OPEN  0x100097f9cull
#define G_DOOR_KEY_CLOSE 0x100097fe4ull
// The platform's input caps (4 instructions): the byte at +0x10 says whether a
// hardware keyboard is there; without one the game's key state reads as all up.
#define G_INPUT_CAPS     0x1001171a8ull
// iOS: the folder for saves and settings, NSSearchPathForDirectoriesInDomains(14 = Application
// Support, ...): `mov w0, #14`, the game's only folder lookup
#define G_SAVE_FOLDER    0x1000b8ee4ull
#define G_PNG_PARSE                                                                                                    \
    0x1000092d0ull // void *png_parse(const void *data, uint32_t size, int mem_flags, int *w, int *h): a stub (the game's)
#define G_MEM_ALLOC                                                                                                    \
    0x1001be780ull // void *mem_alloc(size_t size, unsigned align, int flags): freed with mem_free (the game's)

// ---- game state (iOS touch layer, touch.m; from ftl-linux) ----
#define G_CAPP_PTR             0x100241fc0ull // CApp *
#define G_TOGGLE_PAUSE         0x100180240ull // void (CommandGui *): the pause hotkey's action
#define CAPP_GUI               0x10 // CommandGui *
#define CAPP_MENU              0x20 // MainMenu (first byte: open)
#define CAPP_STARTUP           0x40b8 // language chooser / startup screen up
#define MENU_SUBSCREEN_OFFSETS {0x26b8, 0x3d00, 0x3fd0, 0x26c8, 0x15f0}
#define MENU_CONTINUE_ACTIVE   0x80 // MainMenu: the Continue button is active (there's a saved run)
#define MENU_ITEM              0x450 // MainMenu: int, the button just pressed; CApp acts on it (9: Continue)
#define GUI_STARMAP            0x1e90 // CommandGui: StarMap * (+0x88 int x, y; +0x900 choosing the next sector)
#define GUI_FOCUS_WINDOWS      0x1ea0 // CommandGui: std::vector<FocusWindow *> (FocusWindow +8: bool open)
#define GUI_UPGRADES           0x3cf0 // CommandGui: the ship / crew / cargo window (+0x68 tab)
#define GUI_PAUSED             0x3c58 // CommandGui: bool paused
#define GUI_MODAL_OFFSETS      {0x5948, 0x2248, 0x27e8, 0x3c68, 0x5648, 0x28f8} // +8: open
#define GUI_CHOICE             0x1f68 // CommandGui: ChoiceBox, the event window (+8 open)
#define CHOICE_FRAME           0x28 // ChoiceBox: WindowFrame * (int x, y, w, h)
#define CHOICE_LIST            0x48 // ChoiceBox: std::vector<ChoiceText>, 584-byte entries
#define CHOICE_COLUMN          0x60 // ChoiceBox: unsigned; more choices use a wide layout
#define CHOICE_FONT            0x88 // ChoiceBox: int font size, 10..13
#define CHOICE_CENTRED         0x8c // ChoiceBox: bool
#define CHOICE_RECTS           0x68 // ChoiceBox: std::vector of int x, y, w, h per choice (HUD units)
