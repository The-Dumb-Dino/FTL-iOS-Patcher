#include "util.h"

#include <algorithm>
#include <cctype>

#ifdef _WIN32
#include <windows.h>
#endif

namespace ftl
{

#ifdef _WIN32
static std::wstring Widen(const std::string &s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

static std::string Narrow(const std::wstring &w)
{
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

fs::path U8Path(const std::string &s)
{
    return fs::path(Widen(s));
}
std::string U8Str(const fs::path &p)
{
    return Narrow(p.wstring());
}
FILE *OpenFile(const fs::path &p, const char *mode)
{
    return _wfopen(p.c_str(), Widen(mode).c_str());
}
int Seek64(FILE *f, int64_t off, int whence)
{
    return _fseeki64(f, off, whence);
}
int64_t Tell64(FILE *f)
{
    return _ftelli64(f);
}
#else
fs::path U8Path(const std::string &s)
{
    return fs::path(s);
}
std::string U8Str(const fs::path &p)
{
    return p.string();
}
FILE *OpenFile(const fs::path &p, const char *mode)
{
    return std::fopen(p.c_str(), mode);
}
int Seek64(FILE *f, int64_t off, int whence)
{
    return fseeko(f, off, whence);
}
int64_t Tell64(FILE *f)
{
    return ftello(f);
}
#endif

Bytes ReadFile(const fs::path &p)
{
    FILE *f = OpenFile(p, "rb");
    if (!f) throw Error("can't open " + U8Str(p));
    Seek64(f, 0, SEEK_END);
    int64_t size = Tell64(f);
    Seek64(f, 0, SEEK_SET);
    Bytes data(size_t(size < 0 ? 0 : size));
    size_t got = data.empty() ? 0 : std::fread(data.data(), 1, data.size(), f);
    std::fclose(f);
    if (got != data.size()) throw Error("can't read " + U8Str(p));
    return data;
}

void WriteFile(const fs::path &p, const Bytes &data)
{
    FILE *f = OpenFile(p, "wb");
    if (!f) throw Error("can't write " + U8Str(p));
    size_t put = data.empty() ? 0 : std::fwrite(data.data(), 1, data.size(), f);
    bool ok = put == data.size() && std::fclose(f) == 0;
    if (!ok) throw Error("can't write " + U8Str(p));
}

std::string Lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

}  // namespace ftl
