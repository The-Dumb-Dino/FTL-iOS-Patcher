#include "game.h"

#include <cstring>

#include "macho.h"
#include "prepatch.h"

namespace ftl::game
{

static const uint8_t KNOWN_UUID[16] = {0x87, 0x3d, 0x2b, 0x34, 0xd7, 0x07, 0x3d, 0x6b,
                                       0xab, 0x83, 0x26, 0x7b, 0xf4, 0xc5, 0xbb, 0xc0};  // FTL for iPad 1.6.21, arm64
static const char *MIN_OS = "14.0";
// Stamped as the SDK the game was built with: new iPhones (the iPhone Duo) letterbox
// apps built with older ones. Apps built with it must use scenes (src/ios.m does).
static const char *SDK = "27.2";

std::string Executable(const plist::Dict &info)
{
    auto it = info.find("CFBundleExecutable");
    if (it == info.end() || !it->second.Is<std::string>()) throw Error("Info.plist has no CFBundleExecutable");
    return it->second.As<std::string>();
}

Bytes Thin(const Bytes &binary)
{
    Bytes game = macho::Thin(binary);
    macho::CheckDecrypted(game);
    auto uid = macho::Uuid(game);
    if (!uid || std::memcmp(uid->data(), KNOWN_UUID, 16) != 0)
        throw Error("this isn't FTL for iPad 1.6.21 (arm64 UUID 873d2b34-...), the only build the patches are for");
    return game;
}

Bytes Prepare(const Bytes &game, bool simulator)
{
    Bytes data(game);
    macho::SetBuildVersion(data, simulator ? macho::IOS_SIMULATOR_PLATFORM : macho::IOS_PLATFORM, MIN_OS, SDK);
    macho::AddDylib(data, "@executable_path/Frameworks/ftlios.dylib");
    macho::RemoveSignature(data);
    prepatch::AddSegments(data);
    return data;
}

Bytes Patch(const Bytes &binary, const Bytes &tpl, const std::string &bundleId, bool simulator)
{
    Bytes game = Thin(binary);
    Bytes data = Prepare(game, simulator);
    prepatch::Apply(data, tpl, game);
    macho::AdhocSign(data, bundleId.empty() ? "com.ftlgame.FTL" : bundleId);
    return data;
}

void UpdateInfo(plist::Dict &info, bool simulator)
{
    using namespace plist;
    info["MinimumOSVersion"] = MIN_OS;
    info["UIRequiredDeviceCapabilities"] = Array{"arm64"};
    info["UIDeviceFamily"] = Array{1, 2};  // iPhone and iPad: the game lays itself out for the screen it gets
    // Saves in Documents (src/hooks.c), shown in the Files app
    info["UIFileSharingEnabled"] = true;
    info["LSSupportsOpeningDocumentsInPlace"] = true;
    // The scene lifecycle (src/ios.m)
    info["UIApplicationSceneManifest"] = Dict{
        {"UIApplicationSupportsMultipleScenes", false},
        {"UISceneConfigurations",
         Dict{{"UIWindowSceneSessionRoleApplication", Array{Dict{{"UISceneConfigurationName", "Default Configuration"},
                                                                 {"UISceneDelegateClassName", "FTLSceneDelegate"}}}}}},
    };
    // Long-pressing the icon offers Settings (src/settings.m)
    info["UIApplicationShortcutItems"] = Array{Dict{
        {"UIApplicationShortcutItemType", "ftl.settings"},
        {"UIApplicationShortcutItemTitle", "Settings"},
        {"UIApplicationShortcutItemIconSymbolName", "gearshape"},
    }};
    info["DTSDKName"] = std::string(simulator ? "iphonesimulator" : "iphoneos") + SDK;
    info["DTPlatformVersion"] = SDK;
    if (simulator)
    {
        info["CFBundleSupportedPlatforms"] = Array{"iPhoneSimulator"};
        info["DTPlatformName"] = "iphonesimulator";
    }
}

void SetName(plist::Dict &info, const std::string &name)
{
    info["CFBundleDisplayName"] = name;
    // short name (15 bytes, not cutting a UTF-8 character); the display name is what's shown
    std::string shortname = name.substr(0, 15);
    while (!shortname.empty() && (uint8_t(shortname.back()) & 0xC0) == 0x80) shortname.pop_back();
    if (!shortname.empty() && uint8_t(shortname.back()) >= 0xC0) shortname.pop_back();
    info["CFBundleName"] = shortname;
}

void UseLooseIcons(plist::Dict &info)
{
    for (const char *key : {"CFBundleIcons", "CFBundleIcons~ipad"})
    {
        auto it = info.find(key);
        if (it == info.end() || !it->second.Is<plist::Dict>()) continue;
        auto &d = it->second.As<plist::Dict>();
        auto p = d.find("CFBundlePrimaryIcon");
        if (p != d.end() && p->second.Is<plist::Dict>()) p->second.As<plist::Dict>().erase("CFBundleIconName");
    }
}

}  // namespace ftl::game
