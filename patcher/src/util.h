// Shared helpers: bytes, errors, files with UTF-8 paths on every system.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace ftl
{

namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;
using Log = std::function<void(const std::string &)>;

struct Error : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

inline uint32_t Rd32(const uint8_t *p)
{
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}  // little-endian hosts only
inline uint64_t Rd64(const uint8_t *p)
{
    uint64_t v;
    std::memcpy(&v, p, 8);
    return v;
}
inline void Wr32(uint8_t *p, uint32_t v)
{
    std::memcpy(p, &v, 4);
}
inline void Wr64(uint8_t *p, uint64_t v)
{
    std::memcpy(p, &v, 8);
}
inline uint32_t Rd32Be(const uint8_t *p)
{
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
inline void Wr32Be(uint8_t *p, uint32_t v)
{
    p[0] = v >> 24;
    p[1] = v >> 16;
    p[2] = v >> 8;
    p[3] = uint8_t(v);
}

// Appending little- and big-endian values
inline void Put32(Bytes &b, uint32_t v)
{
    uint8_t t[4];
    Wr32(t, v);
    b.insert(b.end(), t, t + 4);
}
inline void Put64(Bytes &b, uint64_t v)
{
    uint8_t t[8];
    Wr64(t, v);
    b.insert(b.end(), t, t + 8);
}
inline void Put32Be(Bytes &b, uint32_t v)
{
    uint8_t t[4];
    Wr32Be(t, v);
    b.insert(b.end(), t, t + 4);
}
inline void Put64Be(Bytes &b, uint64_t v)
{
    Put32Be(b, uint32_t(v >> 32));
    Put32Be(b, uint32_t(v));
}

// Paths from and to UTF-8 (the GUI, the command line and messages use UTF-8)
fs::path U8Path(const std::string &s);
std::string U8Str(const fs::path &p);

FILE *OpenFile(const fs::path &p, const char *mode);  // nullptr on failure
Bytes ReadFile(const fs::path &p);
void WriteFile(const fs::path &p, const Bytes &data);

// 64-bit seeking
int Seek64(FILE *f, int64_t off, int whence);
int64_t Tell64(FILE *f);

std::string Lower(std::string s);

}  // namespace ftl
