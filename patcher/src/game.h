// The game itself: its binary, checked and patched for the prepatch, and its Info.plist.
// The desktop patcher and the web one (web/patcher.cpp) share this.
#pragma once
#include <string>

#include "plist.h"
#include "util.h"

namespace ftl::game
{

// The binary thinned of other architectures, checked to be the game the patches are for
Bytes Thin(const Bytes &binary);

// Prepared for the prepatch: stamped as built with the current SDK, loading ftlios.dylib,
// unsigned, with the prepatch's segments.
Bytes Prepare(const Bytes &game, bool simulator = false);
// Thin() and Prepare(), then the template (prepatch.ftlt) into it, then ad-hoc signed as
// bundleId (empty: the game's own).
Bytes Patch(const Bytes &binary, const Bytes &tpl, const std::string &bundleId, bool simulator = false);

std::string Executable(const plist::Dict &info);
void UpdateInfo(plist::Dict &info, bool simulator);
// The name under the icon: CFBundleDisplayName, and CFBundleName cut to 15 bytes
// without cutting a UTF-8 character (the display name is what is shown)
void SetName(plist::Dict &info, const std::string &name);
// The loose AppIcon*.png files are the icon: Assets.car's old one wins while
// CFBundleIconName points into it
void UseLooseIcons(plist::Dict &info);

}  // namespace ftl::game
