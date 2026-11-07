// ftl.dat ("PKG\n", big-endian): 16-byte header (magic, header size, entry size,
// entry count, path table size), 20-byte entries (name hash, flags << 24 | path
// offset, data offset, stored size, real size), the NUL-separated path table, then
// the files, zlib-compressed when flag 1 is set.
#include "ftldat.h"

#include <memory>
#include <set>

#include "miniz.h"

namespace ftl::ftldat
{

static const uint8_t MAGIC[8] = {'P', 'K', 'G', '\n', 0, 0x10, 0, 0x14};

namespace
{
struct File
{
    FILE *f;
    explicit File(const fs::path &p, const char *mode) : f(OpenFile(p, mode))
    {
        if (!f) throw Error("can't open " + U8Str(p));
    }
    ~File()
    {
        if (f) std::fclose(f);
    }
    void Read(void *p, size_t n)
    {
        if (n && std::fread(p, 1, n, f) != n) throw Error("ftl.dat is cut short");
    }
    void Write(const void *p, size_t n)
    {
        if (n && std::fwrite(p, 1, n, f) != n) throw Error("can't write ftl.dat");
    }
    void Seek(int64_t off, int whence = SEEK_SET)
    {
        if (Seek64(f, off, whence)) throw Error("can't seek in ftl.dat");
    }
};
}  // namespace

void Fix(const fs::path &path, const Log &log)
{
    File f(path, "r+b");
    uint8_t head[16];
    f.Read(head, 16);
    if (std::memcmp(head, MAGIC, 8) != 0) throw Error(U8Str(path) + " isn't an ftl.dat");
    uint32_t count = Rd32Be(head + 8), psize = Rd32Be(head + 12);
    Bytes index(size_t(count) * 20), table(psize);
    f.Read(index.data(), index.size());
    int64_t tableAt = 16 + int64_t(count) * 20;
    f.Read(table.data(), table.size());

    struct Entry
    {
        size_t i;
        std::string name;
        uint32_t po, flags, off, size, length;
    };
    std::vector<Entry> entries;
    std::set<std::string> names;
    for (uint32_t i = 0; i < count; i++)
    {
        const uint8_t *e = &index[size_t(i) * 20];
        uint32_t fp = Rd32Be(e + 4);
        uint32_t po = fp & 0xFFFFFF;
        if (po >= table.size()) throw Error("broken ftl.dat path table");
        const char *n = reinterpret_cast<const char *>(&table[po]);
        std::string name(n, strnlen(n, table.size() - po));
        entries.push_back({i, name, po, fp >> 24, Rd32Be(e + 8), Rd32Be(e + 12), Rd32Be(e + 16)});
        names.insert(Lower(name));
    }

    // 1. x.tex hidden where a mod added x.png
    int hidden = 0;
    for (auto &e : entries)
    {
        std::string low = Lower(e.name);
        if (low.size() > 4 && low.compare(low.size() - 4, 4, ".tex") == 0 &&
            names.count(low.substr(0, low.size() - 4) + ".png"))
        {
            table[e.po + e.name.size() - 1] = '_';  // x.tex -> x.te_
            hidden++;
        }
    }

    // 2. atlas lists without the images mods put in: the iPad's own art is all .tex, so any
    //    img/*.png in ftl.dat came from a mod
    std::set<std::string> replaced;
    for (auto &n : names)
        if (n.compare(0, 4, "img/") == 0 && n.size() > 4 && n.compare(n.size() - 4, 4, ".png") == 0) replaced.insert(n);
    f.Seek(0, SEEK_END);
    int64_t end = Tell64(f.f);
    int rewritten = 0, dropped = 0;
    for (auto &e : entries)
    {
        std::string low = Lower(e.name);
        if (!(low.compare(0, 6, "atlas/") == 0 && low.size() > 4 && low.compare(low.size() - 4, 4, ".txt") == 0))
            continue;
        f.Seek(e.off);
        Bytes raw(e.size);
        f.Read(raw.data(), raw.size());
        Bytes text;
        if (e.flags & 1)
        {
            text.resize(e.length);
            mz_ulong n = e.length;
            if (mz_uncompress(text.data(), &n, raw.data(), mz_ulong(raw.size())) != MZ_OK || n != e.length)
                throw Error("broken file in ftl.dat: " + e.name);
        }
        else
        {
            text = raw;
        }
        // Lines split on \n (a last empty line stays)
        std::vector<Bytes> lines(1);
        for (uint8_t c : text)
        {
            if (c == '\n')
                lines.emplace_back();
            else
                lines.back().push_back(c);
        }
        std::vector<Bytes *> kept;
        for (auto &l : lines)
        {
            // the first whitespace-separated word, as bytes.split() does
            size_t a = 0;
            auto ws = [](uint8_t c)
            {
                return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
            };
            while (a < l.size() && ws(l[a])) a++;
            size_t b = a;
            while (b < l.size() && !ws(l[b])) b++;
            bool drop = b > a && replaced.count(Lower(std::string(l.begin() + a, l.begin() + b)));
            if (!drop) kept.push_back(&l);
        }
        if (kept.size() == lines.size()) continue;
        dropped += int(lines.size() - kept.size());
        Bytes data;
        for (size_t k = 0; k < kept.size(); k++)
        {
            if (k) data.push_back('\n');
            data.insert(data.end(), kept[k]->begin(), kept[k]->end());
        }
        mz_ulong zlen = mz_compressBound(mz_ulong(data.size()));
        Bytes z(zlen);
        if (mz_compress2(z.data(), &zlen, data.data(), mz_ulong(data.size()), 6) != MZ_OK)
            throw Error("can't compress");
        z.resize(zlen);
        if (end + int64_t(z.size()) > 0xFFFFFFFFLL) throw Error("ftl.dat would grow past 4 GB");
        f.Seek(end);
        f.Write(z.data(), z.size());
        uint8_t *ie = &index[e.i * 20];
        Wr32Be(ie + 8, uint32_t(end));
        Wr32Be(ie + 12, uint32_t(z.size()));
        Wr32Be(ie + 16, uint32_t(data.size()));
        ie[4] |= 1;  // compressed
        end += int64_t(z.size());
        rewritten++;
    }

    f.Seek(16);
    f.Write(index.data(), index.size());
    f.Seek(tableAt);
    f.Write(table.data(), table.size());
    log("  " + std::to_string(hidden) + " iPad textures hidden behind mods' images; " + std::to_string(rewritten) +
        " atlas lists without " + std::to_string(dropped) + " replaced images");
}

bool HasFile(const fs::path &path, const std::string &name)
{
    FILE *f = OpenFile(path, "rb");
    if (!f) return false;
    std::unique_ptr<FILE, int (*)(FILE *)> guard(f, std::fclose);
    uint8_t head[16];
    if (std::fread(head, 1, 16, f) != 16 || std::memcmp(head, MAGIC, 8) != 0) return false;
    uint32_t count = Rd32Be(head + 8), psize = Rd32Be(head + 12);
    if (Seek64(f, 16 + int64_t(count) * 20, SEEK_SET)) return false;
    std::string table(psize, '\0');
    if (std::fread(table.data(), 1, psize, f) != psize) return false;
    std::string want = Lower(name);
    size_t start = 0;
    for (;;)
    {
        size_t z = table.find('\0', start);
        if (Lower(table.substr(start, z == std::string::npos ? std::string::npos : z - start)) == want) return true;
        if (z == std::string::npos) return false;
        start = z + 1;
    }
}

}  // namespace ftl::ftldat
