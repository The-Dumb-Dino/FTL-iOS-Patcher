#include "patch.h"

#include <regex>
#include <set>

#include "ftldat.h"
#include "icon.h"
#include "macho.h"
#include "plist.h"
#include "prepatch.h"
#include "zipio.h"

#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <sys/stat.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace ftl
{

static const uint8_t KNOWN_UUID[16] = {0x87, 0x3d, 0x2b, 0x34, 0xd7, 0x07, 0x3d, 0x6b,
                                       0xab, 0x83, 0x26, 0x7b, 0xf4, 0xc5, 0xbb, 0xc0};  // FTL for iPad 1.6.21, arm64
static const char *MIN_OS = "14.0";
// Stamped as the SDK the game was built with: new iPhones (the iPhone Duo) letterbox
// apps built with older ones. Apps built with it must use scenes (src/ios.m does).
static const char *SDK = "27.2";
static const char *MARKER = ".ftl-ios-patcher";  // in an unpacked folder: the app's name, like FTL.app

static fs::path ExeDir()
{
#ifdef _WIN32
    std::wstring buf(32768, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, buf.data(), DWORD(buf.size()));
    buf.resize(n);
    return fs::path(buf).parent_path();
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buf(size, '\0');
    _NSGetExecutablePath(buf.data(), &size);
    return fs::weakly_canonical(fs::path(buf.c_str())).parent_path();
#else
    return fs::weakly_canonical("/proc/self/exe").parent_path();
#endif
}

fs::path FindFiles()
{
    fs::path d = ExeDir();
    for (fs::path c : {d / "files", d.parent_path() / "Resources" / "files", d.parent_path() / "files"})
        if (fs::is_regular_file(c / DYLIB_NAME)) return c;
    return d / "files";
}

bool IsUnpacked(const fs::path &dir)
{
    return fs::is_regular_file(dir / MARKER);
}

fs::path DefaultFolder(const fs::path &ipa)
{
    return ipa.parent_path() / (ipa.stem().native() + fs::path(" (unpacked)").native());
}

fs::path DefaultOutput(const fs::path &ipa)
{
    return ipa.parent_path() / (ipa.stem().native() + fs::path("-patched.ipa").native());
}

bool HasHyperspace(const fs::path &dir)
{
    return ftldat::HasFile(dir / "ftl.dat", "data/hyperspace.xml");
}

bool ValidBundleId(const std::string &id)
{
    static const std::regex BUNDLE_ID(R"(^[A-Za-z0-9-]+(\.[A-Za-z0-9-]+)+$)");
    return std::regex_match(id, BUNDLE_ID);
}

static void Step(const Log &log, const std::string &text)
{
    log("==> " + text);
}

static plist::Dict ReadInfo(const fs::path &app)
{
    plist::Value v = plist::Parse(ReadFile(app / "Info.plist"));
    if (!v.Is<plist::Dict>()) throw Error("Info.plist isn't a dictionary");
    return v.As<plist::Dict>();
}

static std::string Executable(const plist::Dict &info)
{
    auto it = info.find("CFBundleExecutable");
    if (it == info.end() || !it->second.Is<std::string>()) throw Error("Info.plist has no CFBundleExecutable");
    return it->second.As<std::string>();
}

// The thin binary as it came, checked to be the game the patches are for
static Bytes ReadGame(const fs::path &app, const plist::Dict &info)
{
    Bytes game = macho::Thin(ReadFile(app / U8Path(Executable(info))));
    macho::CheckDecrypted(game);
    auto uid = macho::Uuid(game);
    if (!uid || std::memcmp(uid->data(), KNOWN_UUID, 16) != 0)
        throw Error("this isn't FTL for iPad 1.6.21 (arm64 UUID 873d2b34-...), the only build the patches are for");
    return game;
}

// A Hyperspace build for iOS: a 64-bit arm64 dylib
void CheckHyperspace(const fs::path &dylib)
{
    if (!fs::is_regular_file(dylib)) throw Error("Hyperspace build not found: " + U8Str(dylib));
    FILE *f = OpenFile(dylib, "rb");
    uint8_t h[16] = {};
    size_t n = f ? std::fread(h, 1, sizeof h, f) : 0;
    if (f) std::fclose(f);
    // mach_header_64: magic, cputype (arm64), cpusubtype, filetype (MH_DYLIB = 6)
    if (n < 16 || Rd32(h) != 0xFEEDFACF || Rd32(h + 4) != 0x0100000C || Rd32(h + 12) != 6)
        throw Error(U8Str(dylib.filename()) + " isn't a Hyperspace build for iOS (an arm64 .dylib, like " +
                    HYPERSPACE_NAME + ")");
    // The platform it's built for (LC_BUILD_VERSION): iOS, or the simulator for testing
    Bytes d = ReadFile(dylib);
    uint32_t ncmds = Rd32(&d[16]);
    size_t off = 32;
    for (uint32_t i = 0; i < ncmds && off + 16 <= d.size(); i++)
    {
        uint32_t cmd = Rd32(&d[off]), size = Rd32(&d[off + 4]);
        if (cmd == 0x32)
        {
            uint32_t platform = Rd32(&d[off + 8]);
            if (platform == 1 || platform == 6)
                throw Error(U8Str(dylib.filename()) + " is Hyperspace's macOS build; choose its iOS build "
                                                      "(buildscripts/ios/build.sh device)");
            break;
        }
        if (size < 8) break;
        off += size;
    }
}

std::pair<Bytes, Bytes> PrepareGame(const fs::path &app, bool simulator)
{
    Bytes game = ReadGame(app, ReadInfo(app));
    Bytes data(game);
    macho::SetBuildVersion(data, simulator ? macho::IOS_SIMULATOR_PLATFORM : macho::IOS_PLATFORM, MIN_OS, SDK);
    macho::AddDylib(data, std::string("@executable_path/Frameworks/") + DYLIB_NAME);
    macho::RemoveSignature(data);
    prepatch::AddSegments(data);
    return {game, data};
}

void Unpack(const fs::path &ipa, const fs::path &dir, const Log &log)
{
    if (fs::exists(dir) && !fs::is_empty(dir) && !IsUnpacked(dir))
        throw Error(U8Str(dir) + " already exists and isn't an unpacked FTL; choose another folder");
    fs::path part = dir.parent_path() / (dir.filename().native() + fs::path(".part").native());
    fs::remove_all(part);
    fs::path app;
    Step(log, "Unpacking " + U8Str(ipa.filename()));
    if (fs::is_directory(ipa))
    {  // an .app (macOS keeps them as folders)
        fs::create_directories(part);
        fs::copy(ipa, part / ipa.filename(), fs::copy_options::recursive | fs::copy_options::copy_symlinks);
        app = part / ipa.filename();
    }
    else
    {
        zip::Extract(ipa, part);
        std::vector<fs::path> apps;
        for (fs::path base : {part / "Payload", part})
            if (fs::is_directory(base))
                for (auto &e : fs::directory_iterator(base))
                    if (e.is_directory() && e.path().extension() == ".app") apps.push_back(e.path());
        if (apps.size() != 1)
        {
            fs::remove_all(part);
            throw Error("the archive should have exactly one .app (Payload/FTL.app in an IPA)");
        }
        app = apps[0];
    }
    try
    {
        ReadGame(app, ReadInfo(app));
    }
    catch (...)
    {
        fs::remove_all(part);
        throw;
    }
    // App Store DRM leftovers, and a signature that won't fit any more
    for (const char *leftover : {"SC_Info", "_CodeSignature", "embedded.mobileprovision"})
        fs::remove_all(app / leftover);
    std::string appName = U8Str(app.filename());
    WriteFile(app / MARKER, Bytes(appName.begin(), appName.end()));
    fs::remove_all(dir);
    fs::rename(app, dir);
    fs::remove_all(part);
    log("  unpacked to " + U8Str(dir));
}

static void UpdateInfo(plist::Dict &info, bool simulator)
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

// Every file of the unpacked folder and of `extra`: a folder's files, then its subfolders,
// each sorted
static void Walk(const fs::path &root, const std::string &rel, const std::set<std::string> &extra,
                 std::vector<std::string> &out)
{
    std::set<std::string> files, dirs;
    fs::path here = rel.empty() ? root : root / U8Path(rel);
    if (fs::is_directory(here))
        for (auto &e : fs::directory_iterator(here))
        {
            std::string name = U8Str(e.path().filename());
            (e.is_directory() ? dirs : files).insert(name);
        }
    std::string prefix = rel.empty() ? "" : rel + "/";
    for (auto &x : extra)
    {
        if (x.compare(0, prefix.size(), prefix) != 0) continue;
        std::string rest = x.substr(prefix.size());
        size_t slash = rest.find('/');
        (slash == std::string::npos ? files : dirs).insert(rest.substr(0, slash));
    }
    for (auto &f : files) out.push_back(prefix + f);
    for (auto &d : dirs) Walk(root, prefix + d, extra, out);
}

void Pack(const fs::path &dir, const fs::path &files, const fs::path &out, const PackOptions &opt, const Log &log)
{
    if (!IsUnpacked(dir)) throw Error(U8Str(dir) + " isn't a folder unpacked by the patcher");
    if (!opt.bundleId.empty() && !ValidBundleId(opt.bundleId))
        throw Error(opt.bundleId + ": not a bundle ID (letters, digits, - and dots, like com.example.ftl)");
    if (!opt.icon.empty() && !fs::is_regular_file(opt.icon)) throw Error("icon not found: " + U8Str(opt.icon));
    Bytes marker = ReadFile(dir / MARKER);
    std::string appName(marker.begin(), marker.end());
    if (appName.empty() || appName.find('/') != std::string::npos) appName = "FTL.app";

    bool hs = HasHyperspace(dir);
    fs::path tmpl = files / (hs ? "prepatch-hyperspace.ftlt" : "prepatch.ftlt");
    fs::path hsDylib = files / HYPERSPACE_NAME;
    bool ownTemplate = false;
    if (!opt.hyperspace.empty() && hs)
    {
        CheckHyperspace(opt.hyperspace);
        hsDylib = opt.hyperspace;
        fs::path beside = opt.hyperspace.parent_path() / "prepatch-hyperspace.ftlt";
        if (fs::is_regular_file(beside)) tmpl = beside, ownTemplate = true;
    }
    // Releases don't include Hyperspace: the player brings its iOS build (only development files have it)
    if (hs && !fs::is_regular_file(hsDylib))
        throw Error(std::string("The mods include Hyperspace, so the patcher needs Hyperspace's iOS build (") +
                    HYPERSPACE_NAME + "). Choose it next to Hyperspace, or pass --hyperspace.");
    std::vector<fs::path> needed = {files / DYLIB_NAME, tmpl};
    for (auto &p : needed)
        if (!fs::is_regular_file(p)) throw Error(U8Str(p) + " is missing; use the files of a release (see the README)");

    Step(log,
         std::string("Patching the game, ") + (hs ? "with Hyperspace (the mods include it)" : "without Hyperspace"));
    if (!opt.hyperspace.empty() && !hs) log("  (your Hyperspace build isn't used: the mods don't include Hyperspace)");
    if (!opt.hyperspace.empty() && hs)
        log("  Hyperspace: " + U8Str(opt.hyperspace.filename()) + ", with " +
            (ownTemplate ? "the prepatch next to it" : "the patcher's prepatch (it must have the same hooks)"));
    plist::Dict info = ReadInfo(dir);
    std::string exe = Executable(info);
    auto [game, data] = PrepareGame(dir, opt.simulator);
    size_t words = prepatch::Apply(data, tmpl, game);
    log("  " + std::to_string(words) + " instructions changed, " + (hs ? "with" : "without") + " Hyperspace");

    // Mods' images over the iPad's textures and atlases (changes nothing without mods; can be repeated).
    // ftlman's backup, ftl.dat.vanilla, says mods were applied.
    ftldat::Fix(dir / "ftl.dat", fs::exists(dir / "ftl.dat.vanilla") ? log : Log([](const std::string &) {}));

    UpdateInfo(info, opt.simulator);
    std::map<std::string, Bytes> overrides;
    if (!opt.bundleId.empty()) info["CFBundleIdentifier"] = opt.bundleId;
    if (!opt.name.empty())
    {
        info["CFBundleDisplayName"] = opt.name;
        // short name (15 bytes, not cutting a UTF-8 character); the display name is what's shown
        std::string shortname = opt.name.substr(0, 15);
        while (!shortname.empty() && (uint8_t(shortname.back()) & 0xC0) == 0x80) shortname.pop_back();
        if (!shortname.empty() && uint8_t(shortname.back()) >= 0xC0) shortname.pop_back();
        info["CFBundleName"] = shortname;
    }
    if (!opt.icon.empty())
    {
        auto icons = icon::Make(dir, opt.icon, log);
        // Assets.car has the old icon and wins while CFBundleIconName points into it
        for (const char *key : {"CFBundleIcons", "CFBundleIcons~ipad"})
        {
            auto it = info.find(key);
            if (it == info.end() || !it->second.Is<plist::Dict>()) continue;
            auto &d = it->second.As<plist::Dict>();
            auto p = d.find("CFBundlePrimaryIcon");
            if (p != d.end() && p->second.Is<plist::Dict>()) p->second.As<plist::Dict>().erase("CFBundleIconName");
        }
        log("  icon: " + std::to_string(icons.size()) + " sizes from " + U8Str(opt.icon.filename()));
        for (auto &[n, png] : icons) overrides[n] = std::move(png);
    }
    std::string xml = plist::ToXml(info);
    overrides["Info.plist"] = Bytes(xml.begin(), xml.end());

    auto bid = info.find("CFBundleIdentifier");
    macho::AdhocSign(data, bid != info.end() && bid->second.Is<std::string>() ? bid->second.As<std::string>()
                                                                              : "com.ftlgame.FTL");
    overrides[exe] = std::move(data);
    overrides[std::string("Frameworks/") + DYLIB_NAME] = ReadFile(files / DYLIB_NAME);
    if (hs) overrides[std::string("Frameworks/") + HYPERSPACE_NAME] = ReadFile(hsDylib);

    Step(log, "Packing " + U8Str(out.filename()) + " (this takes a minute)");
    std::set<std::string> extra;
    for (auto &kv : overrides) extra.insert(kv.first);
    std::vector<std::string> entries;
    Walk(dir, "", extra, entries);
    fs::path part = out.parent_path() / (out.filename().native() + fs::path(".part").native());
    {
        zip::Writer z(part);
        for (auto &rel : entries)
        {
            if (rel == MARKER || rel == "ftl.dat.vanilla") continue;
            if (!hs && rel == std::string("Frameworks/") + HYPERSPACE_NAME) continue;
            std::string arc = "Payload/" + appName + "/" + rel;
            fs::path src = dir / U8Path(rel);
            bool exec = rel == exe || fs::path(rel).extension() == ".dylib";
#ifndef _WIN32
            struct stat st;
            if (!exec && !overrides.count(rel) && stat(src.c_str(), &st) == 0) exec = st.st_mode & 0111;
#endif
            auto o = overrides.find(rel);
            if (o != overrides.end())
                z.Add(arc, o->second, exec);
            else
                z.Add(arc, src, exec);
        }
        z.Close();
    }
    fs::rename(part, out);
    log("");
    log("Done: " + U8Str(out));
    log("Install it with Sideloadly, AltStore or SideStore.");
}

}  // namespace ftl
