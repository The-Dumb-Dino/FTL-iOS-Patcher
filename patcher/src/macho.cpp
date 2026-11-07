#include "macho.h"

#include <algorithm>

#include "sha256.h"

namespace ftl::macho
{

constexpr uint32_t MH_MAGIC_64 = 0xFEEDFACF;
constexpr uint32_t FAT_MAGIC = 0xCAFEBABE;
constexpr uint32_t CPU_ARM64 = 0x0100000C;

constexpr uint32_t LC_SEGMENT_64 = 0x19;
constexpr uint32_t LC_SYMTAB = 0x2;
constexpr uint32_t LC_DYSYMTAB = 0xB;
constexpr uint32_t LC_LOAD_DYLIB = 0xC;
constexpr uint32_t LC_UUID = 0x1B;
constexpr uint32_t LC_CODE_SIGNATURE = 0x1D;
constexpr uint32_t LC_DYLD_INFO = 0x22;
constexpr uint32_t LC_DYLD_INFO_ONLY = 0x80000022;
constexpr uint32_t LC_VERSION_MIN_IPHONEOS = 0x25;
constexpr uint32_t LC_ENCRYPTION_INFO_64 = 0x2C;
constexpr uint32_t LC_BUILD_VERSION = 0x32;
// linkedit_data_command: dataoff, datasize
static bool IsLinkeditData(uint32_t cmd)
{
    switch (cmd)
    {
        case 0x1D:
        case 0x1E:
        case 0x26:
        case 0x29:
        case 0x2B:
        case 0x2E:
        case 0x80000033:
        case 0x80000034: return true;
    }
    return false;
}

constexpr uint64_t PAGE = 0x4000;  // arm64 iOS segment alignment

static uint64_t Align(uint64_t n, uint64_t a)
{
    return (n + a - 1) & ~(a - 1);
}

static uint32_t Version(const std::string &text)
{
    uint32_t parts[3] = {0, 0, 0};
    size_t i = 0, start = 0;
    while (i < 3 && start <= text.size())
    {
        size_t dot = text.find('.', start);
        parts[i++] = uint32_t(std::stoul(text.substr(start, dot - start)));
        if (dot == std::string::npos) break;
        start = dot + 1;
    }
    return parts[0] << 16 | parts[1] << 8 | parts[2];
}

struct Cmd
{
    size_t off;
    uint32_t cmd, size;
};

static void Need(const Bytes &d, size_t end)
{
    if (end > d.size()) throw Error("the binary is cut short");
}

static std::vector<Cmd> Commands(const Bytes &d)
{
    Need(d, 32);
    if (Rd32(&d[0]) != MH_MAGIC_64) throw Error("not a 64-bit Mach-O binary");
    uint32_t ncmds = Rd32(&d[16]);
    std::vector<Cmd> out;
    size_t off = 32;
    for (uint32_t i = 0; i < ncmds; i++)
    {
        Need(d, off + 8);
        Cmd c{off, Rd32(&d[off]), Rd32(&d[off + 4])};
        if (c.size < 8) throw Error("broken load command");
        Need(d, off + c.size);
        out.push_back(c);
        off += c.size;
    }
    return out;
}

Bytes Thin(const Bytes &d)
{
    if (d.size() < 8 || Rd32Be(&d[0]) != FAT_MAGIC) return d;
    uint32_t n = Rd32Be(&d[4]);
    for (uint32_t i = 0; i < n; i++)
    {
        size_t at = 8 + 20 * size_t(i);
        Need(d, at + 20);
        if (Rd32Be(&d[at]) == CPU_ARM64)
        {
            uint32_t off = Rd32Be(&d[at + 8]), size = Rd32Be(&d[at + 12]);
            Need(d, size_t(off) + size);
            return Bytes(d.begin() + off, d.begin() + off + size);
        }
    }
    throw Error("no arm64 slice in the binary");
}

std::map<std::string, Segment> Segments(const Bytes &d)
{
    std::map<std::string, Segment> out;
    for (auto &c : Commands(d))
    {
        if (c.cmd != LC_SEGMENT_64) continue;
        const char *name = reinterpret_cast<const char *>(&d[c.off + 8]);
        out[std::string(name, strnlen(name, 16))] =
            Segment{c.off, Rd64(&d[c.off + 24]), Rd64(&d[c.off + 32]), Rd64(&d[c.off + 40]), Rd64(&d[c.off + 48])};
    }
    return out;
}

static Segment FindSegment(const Bytes &d, const char *name)
{
    auto segs = Segments(d);
    auto it = segs.find(name);
    if (it == segs.end()) throw Error(std::string("no ") + name + " segment");
    return it->second;
}

void CheckDecrypted(const Bytes &d)
{
    for (auto &c : Commands(d))
        if (c.cmd == LC_ENCRYPTION_INFO_64 && Rd32(&d[c.off + 16]) != 0)
            throw Error("the binary is still FairPlay-encrypted (cryptid 1); a decrypted IPA is needed");
}

std::optional<std::array<uint8_t, 16>> Uuid(const Bytes &d)
{
    for (auto &c : Commands(d))
        if (c.cmd == LC_UUID)
        {
            std::array<uint8_t, 16> u;
            std::copy(d.begin() + c.off + 8, d.begin() + c.off + 24, u.begin());
            return u;
        }
    return std::nullopt;
}

// Bytes free between the load commands and the first section's data
static int64_t HeaderRoom(const Bytes &d)
{
    uint32_t sizeofcmds = Rd32(&d[20]);
    uint64_t first = d.size();
    for (auto &c : Commands(d))
    {
        if (c.cmd != LC_SEGMENT_64) continue;
        uint32_t nsects = Rd32(&d[c.off + 64]);
        for (uint32_t s = 0; s < nsects; s++)
        {
            uint32_t sectOff = Rd32(&d[c.off + 72 + s * 80 + 48]);
            if (sectOff) first = std::min<uint64_t>(first, sectOff);
        }
    }
    return int64_t(first) - int64_t(32 + sizeofcmds);
}

// Puts `repl` in place of the load command at off (size bytes), moving the ones after it
static void ReplaceCommand(Bytes &d, size_t off, size_t size, const Bytes &repl)
{
    uint32_t ncmds = Rd32(&d[16]), sizeofcmds = Rd32(&d[20]);
    size_t end = 32 + sizeofcmds;
    int64_t grow = int64_t(repl.size()) - int64_t(size);
    if (grow > HeaderRoom(d)) throw Error("no room for more load commands");
    Bytes block(repl);
    block.insert(block.end(), d.begin() + off + size, d.begin() + end);
    block.resize(block.size() + size_t(std::max<int64_t>(-grow, 0)), 0);  // freed bytes stay as padding
    std::copy(block.begin(), block.end(), d.begin() + off);
    Wr32(&d[16], ncmds - (repl.empty() ? 1 : 0));
    Wr32(&d[20], uint32_t(int64_t(sizeofcmds) + grow));
}

static void AppendCommand(Bytes &d, const Bytes &cmd)
{
    uint32_t ncmds = Rd32(&d[16]), sizeofcmds = Rd32(&d[20]);
    if (int64_t(cmd.size()) > HeaderRoom(d)) throw Error("no room for more load commands");
    std::copy(cmd.begin(), cmd.end(), d.begin() + 32 + sizeofcmds);
    Wr32(&d[16], ncmds + 1);
    Wr32(&d[20], sizeofcmds + uint32_t(cmd.size()));
}

void SetBuildVersion(Bytes &d, uint32_t platform, const std::string &minos, const std::string &sdk)
{
    for (auto &c : Commands(d))
    {
        if (c.cmd == LC_BUILD_VERSION)
        {
            Wr32(&d[c.off + 8], platform);
            Wr32(&d[c.off + 12], Version(minos));
            Wr32(&d[c.off + 16], Version(sdk));
            return;
        }
        if (c.cmd == LC_VERSION_MIN_IPHONEOS)
        {
            Bytes repl;
            for (uint32_t v : {LC_BUILD_VERSION, 24u, platform, Version(minos), Version(sdk), 0u}) Put32(repl, v);
            ReplaceCommand(d, c.off, c.size, repl);
            return;
        }
    }
    throw Error("no version load command");
}

void AddDylib(Bytes &d, const std::string &name)
{
    for (auto &c : Commands(d))
    {
        if (c.cmd != LC_LOAD_DYLIB) continue;
        uint32_t nameOff = Rd32(&d[c.off + 8]);
        if (nameOff >= c.size) continue;
        const char *p = reinterpret_cast<const char *>(&d[c.off + nameOff]);
        if (std::string(p, strnlen(p, c.size - nameOff)) == name) return;
    }
    size_t size = size_t(Align(24 + name.size() + 1, 8));
    Bytes cmd;
    for (uint32_t v : {LC_LOAD_DYLIB, uint32_t(size), 24u, 2u, 0x10000u, 0x10000u}) Put32(cmd, v);
    cmd.insert(cmd.end(), name.begin(), name.end());
    cmd.resize(size, 0);
    AppendCommand(d, cmd);
}

void RemoveSignature(Bytes &d)
{
    for (auto &c : Commands(d))
    {
        if (c.cmd != LC_CODE_SIGNATURE) continue;
        uint32_t dataoff = Rd32(&d[c.off + 8]), datasize = Rd32(&d[c.off + 12]);
        ReplaceCommand(d, c.off, c.size, {});
        if (uint64_t(dataoff) + datasize == d.size())
        {
            d.resize(dataoff);
            Segment link = FindSegment(d, "__LINKEDIT");
            uint64_t filesize = dataoff - link.fileoff;
            Wr64(&d[link.cmd + 24], link.vmaddr);
            Wr64(&d[link.cmd + 32], Align(filesize, PAGE));
            Wr64(&d[link.cmd + 40], link.fileoff);
            Wr64(&d[link.cmd + 48], filesize);
        }
        return;
    }
}

std::map<std::string, std::tuple<uint64_t, uint64_t, uint64_t>> AddSegments(Bytes &d,
                                                                            const std::vector<NewSegment> &segs)
{
    for (auto &c : Commands(d))
        if (c.cmd == LC_CODE_SIGNATURE) throw Error("the binary is signed; remove the signature first");
    Segment link = FindSegment(d, "__LINKEDIT");
    if (link.fileoff % PAGE || link.vmaddr % PAGE) throw Error("__LINKEDIT isn't page aligned");
    if (int64_t(72 * segs.size()) > HeaderRoom(d)) throw Error("no room for more load commands");

    std::map<std::string, std::tuple<uint64_t, uint64_t, uint64_t>> placed;
    uint64_t vm = link.vmaddr, fo = link.fileoff;
    Bytes cmds;
    for (auto &s : segs)
    {
        uint64_t size = Align(s.size, PAGE);
        uint32_t p = 0;
        for (char ch : s.prot) p |= ch == 'r' ? 1 : ch == 'w' ? 2 : ch == 'x' ? 4 : 0;
        Put32(cmds, LC_SEGMENT_64);
        Put32(cmds, 72);
        char name[16] = {};
        std::memcpy(name, s.name.data(), std::min<size_t>(16, s.name.size()));
        cmds.insert(cmds.end(), name, name + 16);
        Put64(cmds, vm);
        Put64(cmds, size);
        Put64(cmds, fo);
        Put64(cmds, size);
        Put32(cmds, p);
        Put32(cmds, p);
        Put32(cmds, 0);
        Put32(cmds, 0);
        placed[s.name] = {vm, fo, size};
        vm += size;
        fo += size;
    }
    uint64_t shift = fo - link.fileoff;

    Wr64(&d[link.cmd + 24], vm);
    Wr64(&d[link.cmd + 32], link.vmsize);
    Wr64(&d[link.cmd + 40], fo);
    Wr64(&d[link.cmd + 48], link.filesize);

    auto fix = [&](size_t at)
    {
        uint32_t v = Rd32(&d[at]);
        if (v >= link.fileoff) Wr32(&d[at], uint32_t(v + shift));
    };
    for (auto &c : Commands(d))
    {
        if (c.cmd == LC_DYLD_INFO || c.cmd == LC_DYLD_INFO_ONLY)
        {
            for (int i = 0; i < 5; i++)  // rebase, bind, weak bind, lazy bind, export: (off, size)
                if (Rd32(&d[c.off + 8 + i * 8 + 4])) fix(c.off + 8 + i * 8);
        }
        else if (c.cmd == LC_SYMTAB)
        {
            fix(c.off + 8);
            fix(c.off + 16);
        }
        else if (c.cmd == LC_DYSYMTAB)
        {
            for (size_t field : {32, 40, 48, 56, 64, 72})
                if (Rd32(&d[c.off + field])) fix(c.off + field);
        }
        else if (IsLinkeditData(c.cmd))
        {
            if (Rd32(&d[c.off + 12])) fix(c.off + 8);
        }
    }

    // The segment commands go right before __LINKEDIT's, so segments stay in address order.
    uint32_t ncmds = Rd32(&d[16]), sizeofcmds = Rd32(&d[20]);
    size_t end = 32 + sizeofcmds;
    Bytes block(cmds);
    block.insert(block.end(), d.begin() + link.cmd, d.begin() + end);
    std::copy(block.begin(), block.end(), d.begin() + link.cmd);
    Wr32(&d[16], ncmds + uint32_t(segs.size()));
    Wr32(&d[20], sizeofcmds + uint32_t(cmds.size()));
    d.insert(d.begin() + link.fileoff, size_t(shift), 0);
    return placed;
}

void AdhocSign(Bytes &d, const std::string &identifier)
{
    for (auto &c : Commands(d))
        if (c.cmd == LC_CODE_SIGNATURE) throw Error("already signed");
    const size_t page = 4096;
    size_t dataoff = size_t(Align(d.size(), 16));
    d.resize(dataoff, 0);
    std::string ident = identifier + '\0';
    uint32_t nslots = uint32_t((dataoff + page - 1) / page);
    const uint32_t nspecial = 2;  // -2 requirements, -1 Info.plist (not bound: zeros)
    const uint32_t cdHeader = 88;
    uint32_t hashOff = cdHeader + uint32_t(ident.size()) + nspecial * 32;
    uint32_t cdLen = hashOff + nslots * 32;
    Bytes requirements;
    Put32Be(requirements, 0xFADE0C01);
    Put32Be(requirements, 12);
    Put32Be(requirements, 0);
    uint32_t blobLen = 12 + 2 * 8 + cdLen + uint32_t(requirements.size());
    uint32_t datasize = uint32_t(Align(blobLen, 16));

    // The command and __LINKEDIT's new size first: they're in the hashed pages.
    Bytes cmd;
    for (uint32_t v : {LC_CODE_SIGNATURE, 16u, uint32_t(dataoff), datasize}) Put32(cmd, v);
    AppendCommand(d, cmd);
    Segment link = FindSegment(d, "__LINKEDIT");
    uint64_t filesize = dataoff + datasize - link.fileoff;
    Wr64(&d[link.cmd + 24], link.vmaddr);
    Wr64(&d[link.cmd + 32], Align(filesize, PAGE));
    Wr64(&d[link.cmd + 40], link.fileoff);
    Wr64(&d[link.cmd + 48], filesize);
    Segment text = FindSegment(d, "__TEXT");

    Bytes cd;
    for (uint32_t v : {0xFADE0C02u, cdLen, 0x20400u, 0x2u,  // magic, length, version, flags: adhoc
                       hashOff, cdHeader, nspecial, nslots, uint32_t(dataoff)})
        Put32Be(cd, v);
    for (uint8_t v : {32, 2, 0, 12}) cd.push_back(v);  // hash size, SHA-256, platform, 4K pages
    for (int i = 0; i < 4; i++) Put32Be(cd, 0);  // spare2, scatter, team, spare3
    Put64Be(cd, 0);  // codeLimit64
    Put64Be(cd, text.fileoff);  // exec segment: base, limit, main binary
    Put64Be(cd, text.filesize);
    Put64Be(cd, 1);
    cd.insert(cd.end(), ident.begin(), ident.end());
    auto reqHash = Sha256(requirements.data(), requirements.size());
    cd.insert(cd.end(), reqHash.begin(), reqHash.end());  // slot -2 (requirements), then -1
    cd.resize(cd.size() + 32, 0);
    for (size_t i = 0; i < dataoff; i += page)
    {
        auto h = Sha256(&d[i], std::min(page, dataoff - i));
        cd.insert(cd.end(), h.begin(), h.end());
    }
    if (cd.size() != cdLen) throw Error("code directory size mismatch");
    Bytes blob;
    Put32Be(blob, 0xFADE0CC0);
    Put32Be(blob, blobLen);
    Put32Be(blob, 2);
    Put32Be(blob, 0);
    Put32Be(blob, 28);
    Put32Be(blob, 2);
    Put32Be(blob, 28 + cdLen);
    blob.insert(blob.end(), cd.begin(), cd.end());
    blob.insert(blob.end(), requirements.begin(), requirements.end());
    blob.resize(datasize, 0);
    d.insert(d.end(), blob.begin(), blob.end());
}

}  // namespace ftl::macho
