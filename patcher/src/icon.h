// A new app icon from a PNG or a macOS .icns.
#pragma once
#include <map>

#include "util.h"

namespace ftl::icon
{

// The new AppIcon*.png files of the app (file name -> PNG), at the sizes they are.
std::map<std::string, Bytes> Make(const fs::path &app, const fs::path &image, const Log &log);
// The same from an image's bytes (a PNG or .icns), for the files of an IPA: `names` are
// the app's files, `imageName` what to call the image in messages.
std::map<std::string, Bytes> Make(const std::vector<std::string> &names, const Bytes &image,
                                  const std::string &imageName, const Log &log);

}  // namespace ftl::icon
