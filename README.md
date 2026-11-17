# FTL-iOS-Patcher

This Patcher modified the iPad version of **FTL: Faster Than Light** (1.6.21). Allows FTL to run on **all** iOS devices (iPad, iPhone, Duo), QOL changes for smaller screens, an extra settings menu, save file importing & exporting, basic FTL-Modding, [FTL: Hyperspace](https://github.com/FTL-Hyperspace/FTL-Hyperspace).
The patcher runs on all major desktop platforms and in the browser.

> Not affiliated with Subset Games. Neither the repository nor its releases contain any part of the game! You need your own decrypted ipa (see [Legal](#legal)).

## Features

- **iPhone and iPad:** All iOS evices, even the new iPhone Duo.
- **Touch experience:** (optional) Zooming options for small screens, a magnifier whilst touching, two-finger tap to pause, automatic zoom when certain in-game menus open, larger event text, immediate continue upon launch, dual-screen support for the iPhone Duo.
- **Settings:** export and import saves, Game Center toggle, touch options, log exports, the game's data file. Opened from the home screen's Settings quick action, or by holding two fingers still in the game's main menu.
- **Saves accessible in Files:** Save files can be individually picked in the files app on your device now, no longer in private app files.
- **Game data in Files:** the logs and `ftl.dat` are in the Files app too (On My iPhone › FTL › Logs and Game Data). Apps and computers can read them and replace `ftl.dat`; FTL fixes up mods' images in it each time it starts, so a patched `ftl.dat` from another app works.
- **Modding:** FTL mods can be applied using [ftlman](https://github.com/afishhh/ftlman), [FTL: Hyperspace](https://github.com/FTL-Hyperspace/FTL-Hyperspace) can optionally be enabled too for mods like [FTL: Multiverse](https://ftlmultiverse.boards.net/) or Hyperspace improvements.

Tested on an official Apple iOS 27.2 Simulator for iPhones, iPads and the iPhone Duo aswell as my personal M1 iPad Air, with Hyperspace 1.23.2 and Multiverse 5.5.1.
### First run on virtual iPad:
<img width="601.5" height="493" alt="grafik" src="https://github.com/user-attachments/assets/43b31386-922f-4a5c-b272-bb933d8f6fb6" />

### iPhones:
<img width="303" height="226" alt="grafik" src="https://github.com/user-attachments/assets/75db5bcc-1fb3-4464-9b77-c477a9f8c2bd" />
<img width="303" height="226" alt="grafik" src="https://github.com/user-attachments/assets/83d6266f-ed5b-405a-b4d2-356980c694b3" />


## Supporting the project

This is a hobby project done in my spare time. If you find it useful and want
to say thanks, you can buy me a coffee:

[![ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)]([https://ko-fi.com/DEINNAME](https://ko-fi.com/thedumbdino))

Donations are never expected; Bug reports, logs, and well-written issues are
just as valuable.


## Usage

Requires a **decrypted** FTL IPA with version **1.6.21**, a sideloading tools such as ([Sideloadly](https://sideloadly.io), [AltStore](https://altstore.io) or SideStore), [Esign](https://esign-ios.app/) and iOS 14 or newer. For mods you should also download [ftlman](https://github.com/afishhh/ftlman/releases). Hyperspace isn't included in the patcher: for Hyperspace mods you need its iOS build (`Hyperspace.1.6.21.arm64.dylib`) too.

Download the latest patcher from the [releases](https://github.com/The-Dumb-Dino/FTL-iOS-Patcher/releases) and keep it next to its `files/` folder.

1. **Unpack the Game:** choose your IPA and press Unpack. It's checked and unpacked into `<name> (unpacked)` next to it.
2. **FTL Modding:** (optional) point ftlman's to the unpacked ipa containing the ftl.dat, choose your mods (`Hyperspace.ftl` first when Hyperspace mods are used) and press Apply. Hyperspace is patched in when the mods include it.
3. **Pack IPA:** If the mods include Hyperspace, choose its iOS build next to Hyperspace. Pack the IPA and install `<name>-patched.ipa` with your sideloading tool.

To change mods, apply and then also pack once again. Note that a run might break after changing the mods. `ftl.dat` itself is in the Files app, in `Game Data`; another app can replace or patch it there, and FTL applies the iPad mod fix-up at launch.

**Bundle IDs**: sideloading tools automatically give apps an ID of your own, to allow installing next to the App Store version. To update the installed app with new mods or similar simply sideload it again without changing the boundle ID (each ID has its own saves).

**Other Hyperspace builds**: they work as they are if they have the same hooks as the patcher's prepatch (the same version, or changes that don't add or move hooks). Otherwise put the `prepatch-hyperspace.ftlt` made for it (see [Building](#building)) next to its `.dylib`, or choose it under Prepatch in Advanced (or pass `--prepatch`). On a mismatch, Hyperspace says so when the game starts.

**Command line**: `ftl-ios-patcher unpack FTL.ipa FOLDER`, then `ftl-ios-patcher pack FOLDER OUT.ipa [--name NAME] [--icon IMAGE] [--bundle-id ID] [--hyperspace DYLIB] [--prepatch FILE]`.

**Troubleshooting**: Settings > Save Logs (the session logs are in the Files app under FTL > Logs, Hyperspace's under FTL > Hyperspace). Stuck at "Waiting for Game Center": Settings > Game Center off.

**LiveContainer**: the patched IPA also runs inside [LiveContainer](https://github.com/LiveContainer/LiveContainer); the game is loaded as a guest there, which the patch finds by the game's UUID instead of the process's executable. LiveContainer uses its own Info.plist and quick actions, so the Settings quick action is missing: hold two fingers still in the game's main menu for a moment. The guest app's logs and `ftl.dat` are in its `Documents/Logs` and `Documents/Game Data` (shown in the Files app).

## Web version

The patcher also runs in the browser: **[the-dumb-dino.github.io/FTL-iOS-Patcher](https://the-dumb-dino.github.io/FTL-iOS-Patcher/)**. Choose the same decrypted 1.6.21 IPA, patch it and save the result. The IPA is never uploaded: the game's binary and Info.plist are patched by the same code as the desktop patcher (compiled to WebAssembly), and the other files are copied into the new zip as they are.

In **Advanced** it can also change the name and icon, use another bundle ID, and add Hyperspace to the prepatch: choose its iOS build there. Hyperspace isn't included. Another prepatch template (`.ftlt`) can be given too, for a game or Hyperspace build with other hooks. An IPA whose mods are already in `ftl.dat` works too; the web version copies the dat as it is, without the fix-up step. It needs a current browser (WebAssembly and `DecompressionStream`; Safari 16.4 or newer).

## How it works

1. **The binary** is thinned to arm64, ulifted with a current iOS SDK (new iPhones letterbox older apps), made to load `ftlios.dylib`, without Apple's tools (`patcher/src/macho.cpp`).
2. **`ftlios.dylib`** (`src/`) adds the scene lifecycle, settings, touch controls, phone resolution and texture size limit through the Objective-C runtime.
3. **The prepatch**: iOS doesn't let an app change its code, but the PNG reader for mods, a few fixes and Hyperspace's ~2,400 hooks are code changes. So they go into the game's file beforehand, in two added segments; at run time they're only checked. The prepatch is recorded once per release by running the game on a Mac, and shipped as a **template** whose game words are filled in from your copy, so the release contains no part of the game.

## Building

The patcher (`patcher/`, C++ with [wxWidgets](https://www.wxwidgets.org) and [miniz](https://github.com/richgel999/miniz), fetched by CMake):

```
cmake -S patcher -B patcher/build -DCMAKE_BUILD_TYPE=Release && cmake --build patcher/build
```

For Windows from a Mac or Linux, add `-DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake` (x86_64) or `cmake/mingw-w64-arm64.cmake` with [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) in `PATH` (arm64); `patcher/build-linux.sh [arm64]` builds the Linux versions in Docker.

A release needs a Mac with Xcode (and mingw-w64, llvm-mingw and Docker for the Windows and Linux apps, or `--no-apps`). Build Hyperspace's iOS and macOS builds from the same commit of [FTL-Hyperspace-Dino](https://github.com/The-Dumb-Dino/FTL-Hyperspace-Dino) (`buildscripts/ios/build.sh device`, `buildscripts/macos/build.sh`), then:

```
patcher/build/ftl-ios-release --ipa FTL.ipa --version 1.2.0 \
    --hyperspace-ios <device build>/Hyperspace.1.6.21.arm64.dylib \
    --hyperspace-mac <macOS build>/Hyperspace.1.6.21.arm64.dylib \
    [--hyperspace-sim <simulator build>/Hyperspace.1.6.21.arm64.dylib]
```

It builds `ftlios.dylib`, records and checks both prepatches into `files/`, and zips the apps into `dist/`. Hyperspace dylibs must by built from source currently: [FTL-Hyperspace-Dino](https://github.com/The-Dumb-Dino/FTL-Hyperspace-Dino) or simply use the ones I hand out to you. With `--hyperspace-sim` it also writes `files-simulator/`, for `ftl-ios-patcher pack ... --simulator --files files-simulator`.

## Legal

This project does only contain its own code: no assets of FTL wre re-distributed. *FTL: Faster Than Light* is the property of Subset Games.

## Credits

- [ftl-android](https://github.com/Sidd065/ftl-android) by Sidd065: Got FTL: Hyperspace working for the iOS binary and created the most of the settings which were re-used here originally for android.
- [FTL iPhone Duo patch](https://github.com/twelvehourstime/FTL-iPhone-Duo-Patch) by twelvehourstime: The code was not copied but the features replicated.
- [Hyperspace](https://github.com/FTL-Hyperspace/FTL-Hyperspace) special arm fork sits at [FTL-Hyperspace-Dino](https://github.com/The-Dumb-Dino/FTL-Hyperspace-Dino/tree/iOS-binary-ports).
- [stb_image](https://github.com/nothings/stb) (MIT or public domain), [wxWidgets](https://www.wxwidgets.org) (wxWindows Library Licence), [miniz](https://github.com/richgel999/miniz) (MIT).

## License

GPL-3.0. See [LICENSE](LICENSE).
