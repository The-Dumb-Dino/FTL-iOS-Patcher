#include "zipio.h"

#include <ctime>

#include "miniz.h"

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace ftl::zip
{

// ---- reading (miniz) ----

namespace
{
struct Reader
{
    FILE *f = nullptr;
    mz_zip_archive z{};
    bool ok = false;

    explicit Reader(const fs::path &p)
    {
        f = OpenFile(p, "rb");
        if (!f) return;
        Seek64(f, 0, SEEK_END);
        int64_t size = Tell64(f);
        Seek64(f, 0, SEEK_SET);
        ok = mz_zip_reader_init_cfile(&z, f, mz_uint64(size), 0);
    }
    ~Reader()
    {
        if (ok) mz_zip_reader_end(&z);
        if (f) std::fclose(f);
    }
};

size_t WriteCb(void *opaque, mz_uint64, const void *buf, size_t n)
{
    return std::fwrite(buf, 1, n, static_cast<FILE *>(opaque));
}
}  // namespace

void Extract(const fs::path &archive, const fs::path &out, const Log &progress)
{
    Reader r(archive);
    if (!r.f) throw Error("can't open " + U8Str(archive));
    if (!r.ok) throw Error(U8Str(archive.filename()) + " isn't a zip archive (an IPA is one)");
    fs::create_directories(out);
    fs::path root = fs::weakly_canonical(out);
    mz_uint n = mz_zip_reader_get_num_files(&r.z);
    for (mz_uint i = 0; i < n; i++)
    {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&r.z, i, &st)) throw Error("broken archive entry");
        std::string name = st.m_filename;
        fs::path dest = fs::weakly_canonical(root / U8Path(name));
        auto rel = dest.lexically_relative(root);
        if (rel.empty() || *rel.begin() == "..") throw Error(name + ": outside the archive");
        if (st.m_is_directory)
        {
            fs::create_directories(dest);
            continue;
        }
        fs::create_directories(dest.parent_path());
        FILE *f = OpenFile(dest, "wb");
        if (!f) throw Error("can't write " + U8Str(dest));
        bool good = mz_zip_reader_extract_to_callback(&r.z, i, WriteCb, f, 0);
        good = std::fclose(f) == 0 && good;
        if (!good) throw Error("can't unpack " + name);
#ifndef _WIN32
        if ((st.m_external_attr >> 16) & 0111) chmod(dest.c_str(), 0755);
#endif
        if (progress && n > 20 && i % (n / 10) == 0) progress("");
    }
}

bool HasFile(const fs::path &archive, const std::string &name)
{
    Reader r(archive);
    if (!r.ok) return false;
    std::string want = Lower(name);
    mz_uint n = mz_zip_reader_get_num_files(&r.z);
    for (mz_uint i = 0; i < n; i++)
    {
        char buf[1024];
        mz_zip_reader_get_filename(&r.z, i, buf, sizeof buf);
        if (Lower(buf) == want) return true;
    }
    return false;
}

// ---- writing ----

struct Writer::Impl
{
    FILE *f = nullptr;
    fs::path path;
    struct Entry
    {
        std::string name;
        uint32_t crc, mode;
        uint64_t csize, usize, offset;
    };
    std::vector<Entry> entries;
    uint16_t dosTime = 0, dosDate = 0;
    tdefl_compressor *comp = nullptr;
    uint64_t written = 0;

    void Raw(const void *p, size_t n)
    {
        if (n && std::fwrite(p, 1, n, f) != n) throw Error("can't write " + U8Str(path) + " (disk full?)");
    }
    void U16(uint16_t v)
    {
        uint8_t b[2] = {uint8_t(v), uint8_t(v >> 8)};
        Raw(b, 2);
    }
    void U32(uint32_t v)
    {
        uint8_t b[4];
        Wr32(b, v);
        Raw(b, 4);
    }
    void U64(uint64_t v)
    {
        uint8_t b[8];
        Wr64(b, v);
        Raw(b, 8);
    }

    static mz_bool Put(const void *buf, int len, void *user)
    {
        auto *self = static_cast<Impl *>(user);
        if (std::fwrite(buf, 1, size_t(len), self->f) != size_t(len)) return MZ_FALSE;
        self->written += uint64_t(len);
        return MZ_TRUE;
    }

    // One entry; `next` fills a buffer with the next bytes and returns how many (0 at the end)
    template <class Next> void Add(const std::string &name, bool executable, Next next)
    {
        Entry e{name, 0, uint32_t(0100000 | (executable ? 0755 : 0644)), 0, 0, uint64_t(Tell64(f))};
        bool utf8 = false;
        for (unsigned char c : name) utf8 |= c >= 0x80;
        // Local header, always Zip64: the sizes come in the extra field
        U32(0x04034b50);
        U16(45);
        U16(utf8 ? 0x800 : 0);
        U16(8);
        U16(dosTime);
        U16(dosDate);
        U32(0);
        U32(0xFFFFFFFF);
        U32(0xFFFFFFFF);
        U16(uint16_t(name.size()));
        U16(20);
        Raw(name.data(), name.size());
        U16(1);
        U16(16);
        U64(0);
        U64(0);
        written = 0;
        tdefl_init(comp, Put, this, int(tdefl_create_comp_flags_from_zip_params(6, -15, MZ_DEFAULT_STRATEGY)));
        std::vector<uint8_t> buf(1 << 20);
        mz_ulong crc = MZ_CRC32_INIT;
        for (;;)
        {
            size_t n = next(buf.data(), buf.size());
            if (!n) break;
            crc = mz_crc32(crc, buf.data(), n);
            e.usize += n;
            if (tdefl_compress_buffer(comp, buf.data(), n, TDEFL_NO_FLUSH) != TDEFL_STATUS_OKAY)
                throw Error("can't write " + U8Str(path) + " (disk full?)");
        }
        if (tdefl_compress_buffer(comp, nullptr, 0, TDEFL_FINISH) != TDEFL_STATUS_DONE)
            throw Error("can't write " + U8Str(path) + " (disk full?)");
        e.crc = uint32_t(crc);
        e.csize = written;
        int64_t end = Tell64(f);
        Seek64(f, int64_t(e.offset) + 14, SEEK_SET);
        U32(e.crc);
        Seek64(f, int64_t(e.offset) + 30 + int64_t(name.size()) + 4, SEEK_SET);
        U64(e.usize);
        U64(e.csize);
        Seek64(f, end, SEEK_SET);
        entries.push_back(e);
    }
};

Writer::Writer(const fs::path &path) : impl(new Impl)
{
    impl->path = path;
    impl->f = OpenFile(path, "wb");
    if (!impl->f) throw Error("can't write " + U8Str(path));
    impl->comp = static_cast<tdefl_compressor *>(std::malloc(sizeof(tdefl_compressor)));
    std::time_t now = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    impl->dosTime = uint16_t(tm.tm_hour << 11 | tm.tm_min << 5 | tm.tm_sec / 2);
    impl->dosDate = uint16_t((tm.tm_year - 80) << 9 | (tm.tm_mon + 1) << 5 | tm.tm_mday);
}

Writer::~Writer()
{
    if (impl->f) std::fclose(impl->f);
    std::free(impl->comp);
}

void Writer::Add(const std::string &arcname, const fs::path &src, bool executable)
{
    FILE *in = OpenFile(src, "rb");
    if (!in) throw Error("can't read " + U8Str(src));
    try
    {
        impl->Add(arcname, executable, [&](uint8_t *buf, size_t cap) { return std::fread(buf, 1, cap, in); });
    }
    catch (...)
    {
        std::fclose(in);
        throw;
    }
    std::fclose(in);
}

void Writer::Add(const std::string &arcname, const Bytes &data, bool executable)
{
    size_t pos = 0;
    impl->Add(arcname, executable,
              [&](uint8_t *buf, size_t cap)
              {
                  size_t n = std::min(cap, data.size() - pos);
                  if (n) std::memcpy(buf, data.data() + pos, n);
                  pos += n;
                  return n;
              });
}

void Writer::Close()
{
    auto &I = *impl;
    uint64_t cdStart = uint64_t(Tell64(I.f));
    const uint64_t LIMIT = 0xFFFFFFFF;
    for (auto &e : I.entries)
    {
        bool utf8 = false;
        for (unsigned char c : e.name) utf8 |= c >= 0x80;
        Bytes extra;
        if (e.usize >= LIMIT) Put64(extra, e.usize);
        if (e.csize >= LIMIT) Put64(extra, e.csize);
        if (e.offset >= LIMIT) Put64(extra, e.offset);
        I.U32(0x02014b50);
        I.U16(3 << 8 | 45);
        I.U16(45);
        I.U16(utf8 ? 0x800 : 0);
        I.U16(8);  // made by Unix
        I.U16(I.dosTime);
        I.U16(I.dosDate);
        I.U32(e.crc);
        I.U32(uint32_t(std::min(e.csize, LIMIT)));
        I.U32(uint32_t(std::min(e.usize, LIMIT)));
        I.U16(uint16_t(e.name.size()));
        I.U16(uint16_t(extra.empty() ? 0 : extra.size() + 4));
        I.U16(0);
        I.U16(0);
        I.U16(0);
        I.U32(e.mode << 16);
        I.U32(uint32_t(std::min(e.offset, LIMIT)));
        I.Raw(e.name.data(), e.name.size());
        if (!extra.empty())
        {
            I.U16(1);
            I.U16(uint16_t(extra.size()));
            I.Raw(extra.data(), extra.size());
        }
    }
    uint64_t cdEnd = uint64_t(Tell64(I.f));
    uint64_t cdSize = cdEnd - cdStart, count = I.entries.size();
    if (count >= 0xFFFF || cdStart >= LIMIT || cdSize >= LIMIT)
    {
        I.U32(0x06064b50);
        I.U64(44);
        I.U16(3 << 8 | 45);
        I.U16(45);
        I.U32(0);
        I.U32(0);
        I.U64(count);
        I.U64(count);
        I.U64(cdSize);
        I.U64(cdStart);
        I.U32(0x07064b50);
        I.U32(0);
        I.U64(cdEnd);
        I.U32(1);
    }
    I.U32(0x06054b50);
    I.U16(0);
    I.U16(0);
    I.U16(uint16_t(std::min<uint64_t>(count, 0xFFFF)));
    I.U16(uint16_t(std::min<uint64_t>(count, 0xFFFF)));
    I.U32(uint32_t(std::min(cdSize, LIMIT)));
    I.U32(uint32_t(std::min(cdStart, LIMIT)));
    I.U16(0);
    bool good = std::fclose(I.f) == 0;
    I.f = nullptr;
    if (!good) throw Error("can't write " + U8Str(I.path) + " (disk full?)");
}

}  // namespace ftl::zip
