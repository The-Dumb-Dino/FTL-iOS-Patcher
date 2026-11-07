// Zip archives: unpacking IPAs (miniz) and writing them (our own writer, so
// executables keep their Unix mode and big files get Zip64).
#pragma once
#include <memory>

#include "util.h"

namespace ftl::zip
{

// Unpacks everything into `out`; executables keep their mode.
void Extract(const fs::path &archive, const fs::path &out, const Log &progress = nullptr);
// Whether the archive has a file `name` (case doesn't matter); false if it isn't a zip.
bool HasFile(const fs::path &archive, const std::string &name);

class Writer
{
public:
    explicit Writer(const fs::path &path);
    ~Writer();
    void Add(const std::string &arcname, const fs::path &src, bool executable);
    void Add(const std::string &arcname, const Bytes &data, bool executable);
    void Close();

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

}  // namespace ftl::zip
