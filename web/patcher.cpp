// The patcher for the browser (web/): the desktop patcher's core behind a small C API.
// Bytes cross as malloc'd buffers (free them with ftl_free), strings as malloc'd UTF-8.
// Every call returns NULL and leaves a message in ftl_error() on failure.
#include <cstdlib>
#include <cstring>
#include <string>

#include "game.h"
#include "icon.h"
#include "plist.h"

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#define KEEP EMSCRIPTEN_KEEPALIVE
#else
#define KEEP
#endif

namespace
{

std::string gError;

uint8_t *Copy(const ftl::Bytes &bytes, size_t *outLen)
{
    *outLen = bytes.size();
    if (bytes.empty()) return nullptr;
    auto *p = static_cast<uint8_t *>(std::malloc(bytes.size()));
    if (!p)
    {
        gError = "out of memory";
        return nullptr;
    }
    std::memcpy(p, bytes.data(), bytes.size());
    return p;
}

uint8_t *Copy(const std::string &s, size_t *outLen)
{
    return Copy(ftl::Bytes(s.begin(), s.end()), outLen);
}

// The value of an Info.plist key that is a string, or nullptr
uint8_t *PlistString(const uint8_t *plist, size_t len, const char *key, size_t *outLen)
{
    if (outLen) *outLen = 0;
    try
    {
        ftl::plist::Value v = ftl::plist::Parse(ftl::Bytes(plist, plist + len));
        if (!v.Is<ftl::plist::Dict>()) throw ftl::Error("Info.plist isn't a dictionary");
        auto &info = v.As<ftl::plist::Dict>();
        auto it = info.find(key);
        if (it == info.end() || !it->second.Is<std::string>()) return nullptr;
        return Copy(it->second.As<std::string>(), outLen);
    }
    catch (const std::exception &e)
    {
        gError = e.what();
        return nullptr;
    }
}

}  // namespace

extern "C"
{

    KEEP const char *ftl_error(void)
    {
        return gError.c_str();
    }

    KEEP void ftl_free(void *p)
    {
        std::free(p);
    }

// "CFBundleExecutable" or "CFBundleIdentifier" of an Info.plist
    KEEP uint8_t *ftl_plist_string(const uint8_t *plist, size_t len, const char *key, size_t *outLen)
    {
        gError.clear();
        return PlistString(plist, len, key, outLen);
    }

// The Info.plist as the patched app needs it: bundleId, name empty for the game's own;
// looseIcons when the AppIcon*.png files are replaced
    KEEP uint8_t *ftl_patch_info(const uint8_t *plist, size_t len, const char *bundleId, const char *name,
                                 int looseIcons, int simulator, size_t *outLen)
    {
        gError.clear();
        try
        {
            ftl::plist::Value v = ftl::plist::Parse(ftl::Bytes(plist, plist + len));
            if (!v.Is<ftl::plist::Dict>()) throw ftl::Error("Info.plist isn't a dictionary");
            auto &info = v.As<ftl::plist::Dict>();
            ftl::game::UpdateInfo(info, simulator != 0);
            if (bundleId && *bundleId) info["CFBundleIdentifier"] = std::string(bundleId);
            if (name && *name) ftl::game::SetName(info, name);
            if (looseIcons) ftl::game::UseLooseIcons(info);
            return Copy(ftl::plist::ToXml(v), outLen);
        }
        catch (const std::exception &e)
        {
            gError = e.what();
            return nullptr;
        }
    }

// The app's AppIcon*.png files (names separated by newlines) replaced by the PNG or .icns
// image: a packed list (little-endian) of the count, then each name and its PNG, both
// length-prefixed
    KEEP uint8_t *ftl_icons(const char *names, const char *imageName, const uint8_t *image, size_t imageLen,
                            size_t *outLen)
    {
        gError.clear();
        try
        {
            std::vector<std::string> files;
            for (std::string all(names); !all.empty();)
            {
                size_t nl = all.find('\n');
                std::string name = all.substr(0, nl);
                if (!name.empty()) files.push_back(std::move(name));
                if (nl == std::string::npos) break;
                all.erase(0, nl + 1);
            }
            auto icons = ftl::icon::Make(files, ftl::Bytes(image, image + imageLen),
                                         imageName && *imageName ? imageName : "icon", [](const std::string &) {});
            ftl::Bytes packed;
            ftl::Put32(packed, uint32_t(icons.size()));
            for (auto &[name, png] : icons)
            {
                ftl::Put32(packed, uint32_t(name.size()));
                packed.insert(packed.end(), name.begin(), name.end());
                ftl::Put32(packed, uint32_t(png.size()));
                packed.insert(packed.end(), png.begin(), png.end());
            }
            return Copy(packed, outLen);
        }
        catch (const std::exception &e)
        {
            gError = e.what();
            return nullptr;
        }
        catch (...)
        {
            gError = "unknown error";
            return nullptr;
        }
    }

// The game's binary, patched: thin, checked, stamped, loading ftlios.dylib, prepatched and
// signed as bundleId (empty: the game's own).
    KEEP uint8_t *ftl_patch_exe(const uint8_t *exe, size_t exeLen, const uint8_t *tpl, size_t tplLen,
                                const char *bundleId, int simulator, size_t *outLen)
    {
        gError.clear();
        try
        {
            ftl::Bytes binary(exe, exe + exeLen), tmpl(tpl, tpl + tplLen);
            return Copy(ftl::game::Patch(binary, tmpl, bundleId ? bundleId : "", simulator != 0), outLen);
        }
        catch (const std::exception &e)
        {
            gError = e.what();
            return nullptr;
        }
        catch (...)
        {
            gError = "unknown error";
            return nullptr;
        }
    }

}  // extern "C"
