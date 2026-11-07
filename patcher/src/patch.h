// The patcher in two steps, with mods in between:
//   unpack(): the IPA into a plain folder, checked to be the right game;
//   (the player applies mods to that folder with ftlman, if they want)
//   pack():   patches the game into a new IPA. With Hyperspace exactly when the
//             mods include it (data/hyperspace.xml in ftl.dat).
// The folder's game files are left as they are (ftl.dat gets the iPad fix-up,
// which can be repeated), so it can be packed again after changing mods.
#pragma once
#include <utility>

#include "util.h"

namespace ftl
{

inline constexpr const char *DYLIB_NAME = "ftlios.dylib";  // in the app's Frameworks/
inline constexpr const char *HYPERSPACE_NAME = "Hyperspace.1.6.21.arm64.dylib";

struct PackOptions
{
    std::string name;       // on the home screen; empty: unchanged
    std::string bundleId;  // empty: unchanged
    fs::path icon;          // PNG or .icns; empty: unchanged
    // Hyperspace's iOS build, needed when the mods include Hyperspace (releases don't ship it;
    // empty: files/ must have it, as development files do). Its prepatch template,
    // prepatch-hyperspace.ftlt, is taken from next to it if it's there, else the release's,
    // which fits a build with the same hooks.
    fs::path hyperspace;
    bool simulator = false; // development: for the iOS Simulator (with simulator files)
};

// The release's files (ftlios.dylib, Hyperspace, the prepatch templates) next to the program
fs::path FindFiles();
bool IsUnpacked(const fs::path &dir);
fs::path DefaultFolder(const fs::path &ipa);   // "<name> (unpacked)" next to the IPA
fs::path DefaultOutput(const fs::path &ipa);   // "<name>-patched.ipa" next to the IPA
bool HasHyperspace(const fs::path &dir);
bool ValidBundleId(const std::string &id);
void CheckHyperspace(const fs::path &dylib);  // throws if it isn't an iOS arm64 dylib

// The game's binary from an unpacked folder: (the thin binary as it came, prepared for the
// prepatch: stamped for the current SDK, loading ftlios.dylib, unsigned, with its segments)
std::pair<Bytes, Bytes> PrepareGame(const fs::path &app, bool simulator = false);

void Unpack(const fs::path &ipa, const fs::path &dir, const Log &log);
void Pack(const fs::path &dir, const fs::path &files, const fs::path &out, const PackOptions &opt, const Log &log);

}  // namespace ftl
