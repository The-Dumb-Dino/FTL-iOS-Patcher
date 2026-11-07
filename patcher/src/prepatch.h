// The prepatch: every change ftlios.dylib and Hyperspace make to the game's code, put
// into the game's file beforehand, because iOS doesn't let an app change its code while
// it runs (src/prepatch.c has the runtime side).
//
// The binary gets two segments: __HSTEXT for the hook trampolines, __HSDATA for the
// pointers they jump through. A recording (made by the release tool, which runs the
// game once on a Mac) lists the instruction words changed in the game's __TEXT and the
// whole of __HSTEXT. It has game code in it (the trampolines copy the first instruction
// of each hooked function), so what's published is a template: every such word is a
// reference ("the word at this offset in the game"), filled in from the player's copy.
#pragma once
#include <utility>
#include <vector>

#include "util.h"

namespace ftl::prepatch
{

void AddSegments(Bytes &data);
// Puts the template into `data` (prepared: segments added, not signed), filling the
// references from `game` (the thin binary as it came). Returns the words changed in __TEXT.
size_t Apply(Bytes &data, const fs::path &templatePath, const Bytes &game);

struct Recording
{
    std::vector<std::pair<uint32_t, uint32_t>> words;  // (offset in the image, value)
    Bytes code;                                         // __HSTEXT
};
Recording ReadRecording(const fs::path &path);
// A template from a recording. game: the thin, unpatched binary (where the game's words
// come from); prepared: the binary the recording was made for (segments added).
// Returns (words, references).
std::pair<size_t, size_t> MakeTemplate(const Recording &rec, const Bytes &game, const Bytes &prepared,
                                       const fs::path &outPath);

}  // namespace ftl::prepatch
