// Make mods' images show in FTL for iPad: ftl.dat fixed up after ftlman applied mods.
// Mods add PC-style .png images, but the iPad build
//   - looks for its own x.tex texture before x.png, so a mod's x.png is never used, and
//   - keeps small images in atlases, listed in atlas/*.txt: an image listed there is drawn
//     from the atlas, the iPad original, whatever the mod put in.
// So, as ftl-linux does at run time (native/src/ux/mods.c), this hides x.tex where a mod
// added x.png (renamed in place to x.te_; the index stays as it is) and rewrites the atlas
// lists without the images mods replaced (the new lists are appended to the file, and
// their index entries point there).
#pragma once
#include "util.h"

namespace ftl::ftldat
{

void Fix(const fs::path &path, const Log &log);
bool HasFile(const fs::path &path, const std::string &name);

}  // namespace ftl::ftldat
