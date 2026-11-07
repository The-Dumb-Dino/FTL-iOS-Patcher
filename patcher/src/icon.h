// A new app icon from a PNG or a macOS .icns.
#pragma once
#include <map>

#include "util.h"

namespace ftl::icon
{

// The new AppIcon*.png files of the app (file name -> PNG), at the sizes they are.
std::map<std::string, Bytes> Make(const fs::path &app, const fs::path &image, const Log &log);

}  // namespace ftl::icon
