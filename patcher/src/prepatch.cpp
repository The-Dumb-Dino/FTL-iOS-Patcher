// Template file (little-endian):
//   "FTLTMPL\0", u32 version (1), u32 word count, u32 reference count,
//   u32 __HSTEXT size, u64 __HSTEXT vmaddr, u64 __HSDATA vmaddr, 16-byte game UUID,
//   words:      u32 offset in the image, u32 value (0 when it's a reference)
//   references: u32 kind (0: a word of __TEXT, 1: a word of __HSTEXT), u32 where,
//               u32 game offset to copy from
//   __HSTEXT, with zeros where the references go.
#include "prepatch.h"

#include <map>
#include <set>
#include <unordered_map>

#include "macho.h"

namespace ftl::prepatch
{

void AddSegments(Bytes &data)
{
    macho::AddSegments(data, {{"__HSTEXT", 0x80000, "rx"}, {"__HSDATA", 0x10000, "rw"}});
}

size_t Apply(Bytes &data, const fs::path &templatePath, const Bytes &game)
{
    Bytes t = ReadFile(templatePath);
    if (t.size() < 56 || std::memcmp(t.data(), "FTLTMPL\0", 8) != 0)
        throw Error(U8Str(templatePath.filename()) + " isn't a prepatch template");
    uint32_t version = Rd32(&t[8]), nwords = Rd32(&t[12]), nrefs = Rd32(&t[16]), codeSize = Rd32(&t[20]);
    uint64_t hsVm = Rd64(&t[24]), hdVm = Rd64(&t[32]);
    if (version != 1) throw Error("unknown template version " + std::to_string(version));
    auto uid = macho::Uuid(data);
    if (!uid || std::memcmp(uid->data(), &t[40], 16) != 0)
        throw Error("this template is for another build of the game");
    auto segs = macho::Segments(data);
    if (!segs.count("__HSTEXT") || !segs.count("__HSDATA") || !segs.count("__TEXT") ||
        segs["__HSTEXT"].vmaddr != hsVm || segs["__HSTEXT"].filesize != codeSize || segs["__HSDATA"].vmaddr != hdVm)
        throw Error("the binary's prepatch segments don't match the template");
    size_t at = 56;
    if (t.size() < at + size_t(nwords) * 8 + size_t(nrefs) * 12 + codeSize) throw Error("the template is cut short");
    std::vector<std::pair<uint32_t, uint32_t>> words(nwords);
    std::unordered_map<uint32_t, size_t> wordAt;
    for (uint32_t i = 0; i < nwords; i++, at += 8)
    {
        words[i] = {Rd32(&t[at]), Rd32(&t[at + 4])};
        wordAt[words[i].first] = i;
    }
    struct Ref
    {
        uint32_t kind, where, src;
    };
    std::vector<Ref> refs(nrefs);
    for (uint32_t i = 0; i < nrefs; i++, at += 12) refs[i] = {Rd32(&t[at]), Rd32(&t[at + 4]), Rd32(&t[at + 8])};
    Bytes code(t.begin() + at, t.begin() + at + codeSize);

    uint64_t textOff = segs["__TEXT"].fileoff, textSize = segs["__TEXT"].filesize;
    for (auto &r : refs)
    {
        if (uint64_t(r.src) + 4 > textSize || r.src + 4 > game.size())
            throw Error("a template reference is outside the game");
        uint32_t value = Rd32(&game[r.src]);
        if (r.kind == 0)
        {
            auto it = wordAt.find(r.where);
            if (it == wordAt.end()) throw Error("a template reference points to no word");
            words[it->second].second = value;
        }
        else
        {
            if (uint64_t(r.where) + 4 > code.size()) throw Error("a template reference is outside __HSTEXT");
            Wr32(&code[r.where], value);
        }
    }
    for (auto &[offset, value] : words)
    {
        if (uint64_t(offset) + 4 > textSize) throw Error("template word outside __TEXT");
        Wr32(&data[textOff + offset], value);
    }
    std::copy(code.begin(), code.end(), data.begin() + segs["__HSTEXT"].fileoff);
    return words.size();
}

Recording ReadRecording(const fs::path &path)
{
    // From prepatch.c: "FTLPATCH", u32 version (1), u32 word count, u32 __HSTEXT size,
    // words (u32 offset, u32 value), then __HSTEXT
    Bytes p = ReadFile(path);
    if (p.size() < 20 || std::memcmp(p.data(), "FTLPATCH", 8) != 0)
        throw Error(U8Str(path) + " isn't a prepatch recording");
    uint32_t version = Rd32(&p[8]), count = Rd32(&p[12]), codeSize = Rd32(&p[16]);
    if (version != 1) throw Error("unknown recording version " + std::to_string(version));
    size_t at = 20;
    if (p.size() < at + size_t(count) * 8 + codeSize) throw Error("the recording is cut short");
    Recording rec;
    for (uint32_t i = 0; i < count; i++, at += 8) rec.words.push_back({Rd32(&p[at]), Rd32(&p[at + 4])});
    rec.code.assign(p.begin() + at, p.begin() + at + codeSize);
    return rec;
}

std::pair<size_t, size_t> MakeTemplate(const Recording &rec, const Bytes &game, const Bytes &prepared,
                                       const fs::path &outPath)
{
    const size_t ORIGINALS = 0x4000;  // at the end of __HSTEXT: Hyperspace's table of first instructions
    auto segs = macho::Segments(prepared);
    uint64_t hsVm = segs["__HSTEXT"].vmaddr, hsSize = segs["__HSTEXT"].filesize;
    if (hsSize != rec.code.size()) throw Error("the recording doesn't fit the binary's __HSTEXT");
    uint64_t textSize = macho::Segments(game)["__TEXT"].filesize;
    auto gameWord = [&](uint64_t offset)
    {
        return Rd32(&game[offset]);
    };

    // Where game code may have come from: the hooked functions' first instructions (the
    // table at the end of __HSTEXT: u32 offset, u32 instruction), and the words around
    // every changed word (instructions moved around, like the input caps)
    std::set<uint32_t> sources;
    for (size_t at = rec.code.size() - ORIGINALS; at < rec.code.size(); at += 8)
    {
        uint32_t offset = Rd32(&rec.code[at]), ins = Rd32(&rec.code[at + 4]);
        if (!offset) break;
        if (offset + 4 > textSize) throw Error("the recording's table points outside the game");
        if (gameWord(offset) == ins) sources.insert(offset);  // else ftlios.dylib patched it first: no game code
    }
    for (auto &[offset, value] : rec.words)
        for (int d = -16; d < 20; d += 4)
            if (int64_t(offset) + d >= 0 && int64_t(offset) + d + 4 <= int64_t(textSize))
                sources.insert(uint32_t(offset + d));
    std::map<uint32_t, uint32_t> byValue;  // the first (lowest) offset of each word value
    for (uint32_t s : sources) byValue.emplace(gameWord(s), s);

    struct Ref
    {
        uint32_t kind, where, src;
    };
    std::vector<Ref> refs;
    std::vector<std::pair<uint32_t, uint32_t>> outWords;
    for (auto &[offset, value] : rec.words)
    {
        auto it = byValue.find(value);
        if (it != byValue.end())
        {
            refs.push_back({0, offset, it->second});
            outWords.push_back({offset, 0});
        }
        else
        {
            outWords.push_back({offset, value});
        }
    }
    Bytes blob(rec.code);
    for (size_t at = 0; at + 4 <= blob.size(); at += 4)
    {
        uint32_t v = Rd32(&blob[at]);
        auto it = v ? byValue.find(v) : byValue.end();
        if (it != byValue.end())
        {
            refs.push_back({1, uint32_t(at), it->second});
            Wr32(&blob[at], 0);
        }
    }

    Bytes out = {'F', 'T', 'L', 'T', 'M', 'P', 'L', 0};
    for (uint32_t v : {1u, uint32_t(outWords.size()), uint32_t(refs.size()), uint32_t(blob.size())}) Put32(out, v);
    Put64(out, hsVm);
    Put64(out, segs["__HSDATA"].vmaddr);
    auto uid = macho::Uuid(prepared);
    if (!uid) throw Error("the binary has no UUID");
    out.insert(out.end(), uid->begin(), uid->end());
    for (auto &[offset, value] : outWords) Put32(out, offset), Put32(out, value);
    for (auto &r : refs) Put32(out, r.kind), Put32(out, r.where), Put32(out, r.src);
    out.insert(out.end(), blob.begin(), blob.end());
    WriteFile(outPath, out);
    return {outWords.size(), refs.size()};
}

}  // namespace ftl::prepatch
