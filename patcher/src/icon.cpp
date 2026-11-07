// The game ships its icon twice: in Assets.car (compiled, found through
// CFBundleIconName) and as loose AppIcon*.png files (CFBundleIconFiles). Compiling an
// asset catalog needs Xcode, so the patcher replaces the loose files and drops
// CFBundleIconName so iOS uses them.
#include "icon.h"

#include <cmath>
#include <regex>

#include "miniz.h"
#include "stb_image.h"

namespace ftl::icon
{

namespace
{
struct Pixel
{
    double r, g, b, a;
};
using Image = std::vector<std::vector<Pixel>>;  // rows

Image FromRgba(const uint8_t *p, int w, int h)
{
    Image img(size_t(h), std::vector<Pixel>(size_t(w), Pixel{0, 0, 0, 0}));
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
        {
            const uint8_t *q = p + (size_t(y) * w + x) * 4;
            img[y][x] = {q[0] / 255.0, q[1] / 255.0, q[2] / 255.0, q[3] / 255.0};
        }
    return img;
}

Image DecodePng(const uint8_t *data, size_t n, const std::string &what)
{
    int w, h, ch;
    stbi_convert_iphone_png_to_rgb(1);  // Apple's optimized PNGs too
    stbi_set_unpremultiply_on_load(1);
    uint8_t *p = stbi_load_from_memory(data, int(n), &w, &h, &ch, 4);
    if (!p) throw Error(what + ": can't read this image (" + stbi_failure_reason() + ")");
    Image img = FromRgba(p, w, h);
    stbi_image_free(p);
    return img;
}

// Apple's icns RLE: each channel separately; a byte < 128 copies that + 1 bytes,
// one >= 128 repeats the next byte (that - 125) times.
std::vector<Bytes> UnpackRle(const uint8_t *d, size_t n, size_t count, int channels)
{
    std::vector<Bytes> out;
    size_t pos = 0;
    for (int c = 0; c < channels; c++)
    {
        Bytes chan;
        while (chan.size() < count && pos < n)
        {
            uint8_t b = d[pos];
            if (b < 128)
            {
                size_t len = std::min<size_t>(b + 1, n - pos - 1);
                chan.insert(chan.end(), d + pos + 1, d + pos + 1 + len);
                pos += 2 + b;
            }
            else
            {
                if (pos + 1 >= n) break;
                chan.insert(chan.end(), size_t(b - 125), d[pos + 1]);
                pos += 2;
            }
        }
        if (chan.size() < count) throw Error("broken .icns image");
        chan.resize(count);
        out.push_back(std::move(chan));
    }
    return out;
}

Image ReadIcns(const Bytes &d, const std::string &what)
{
    std::map<std::string, std::pair<size_t, size_t>> entries;  // type -> (offset, length) of the body
    size_t total = std::min<size_t>(d.size(), Rd32Be(&d[4])), pos = 8;
    while (pos + 8 <= total)
    {
        std::string kind(reinterpret_cast<const char *>(&d[pos]), 4);
        size_t len = Rd32Be(&d[pos + 4]);
        if (len < 8 || pos + len > d.size()) break;
        entries[kind] = {pos + 8, len - 8};
        pos += len;
    }
    // PNGs: 16 to 1024 pixels in current files; the largest
    size_t best = 0, bestW = 0, bestLen = 0;
    for (auto &[k, e] : entries)
    {
        if (e.second >= 24 && std::memcmp(&d[e.first], "\x89PNG\r\n\x1a\n", 8) == 0)
        {
            size_t w = Rd32Be(&d[e.first + 16]);
            if (w > bestW) bestW = w, best = e.first, bestLen = e.second;
        }
    }
    if (bestW) return DecodePng(&d[best], bestLen, what);
    // Older images: RLE-packed RGB with a separate 8-bit mask, or ARGB
    struct Rle
    {
        const char *kind;
        int size;
        const char *mask;
    };
    for (Rle r : {Rle{"it32", 128, "t8mk"}, Rle{"ih32", 48, "h8mk"}, Rle{"il32", 32, "l8mk"}, Rle{"is32", 16, "s8mk"}})
    {
        auto it = entries.find(r.kind);
        if (it == entries.end()) continue;
        size_t skip = std::string(r.kind) == "it32" ? 4 : 0;
        size_t count = size_t(r.size) * r.size;
        auto rgb = UnpackRle(&d[it->second.first + skip], it->second.second - skip, count, 3);
        Bytes alpha(count, 255);
        auto m = entries.find(r.mask);
        if (m != entries.end() && m->second.second >= count)
            alpha.assign(d.begin() + m->second.first, d.begin() + m->second.first + count);
        Bytes rgba(count * 4);
        for (size_t i = 0; i < count; i++)
            rgba[4 * i] = rgb[0][i], rgba[4 * i + 1] = rgb[1][i], rgba[4 * i + 2] = rgb[2][i],
                     rgba[4 * i + 3] = alpha[i];
        return FromRgba(rgba.data(), r.size, r.size);
    }
    for (auto [kind, size] : {std::pair{"ic05", 32}, std::pair{"ic04", 16}})
    {
        auto it = entries.find(kind);
        if (it == entries.end() || it->second.second < 4 || std::memcmp(&d[it->second.first], "ARGB", 4) != 0) continue;
        size_t count = size_t(size) * size;
        auto argb = UnpackRle(&d[it->second.first + 4], it->second.second - 4, count, 4);
        Bytes rgba(count * 4);
        for (size_t i = 0; i < count; i++)
            rgba[4 * i] = argb[1][i], rgba[4 * i + 1] = argb[2][i], rgba[4 * i + 2] = argb[3][i],
                     rgba[4 * i + 3] = argb[0][i];
        return FromRgba(rgba.data(), size, size);
    }
    throw Error(what + ": no image in this .icns that the patcher can read (only JPEG 2000 ones?)");
}

Image ReadImage(const fs::path &path)
{
    Bytes d = ReadFile(path);
    std::string what = U8Str(path.filename());
    if (d.size() >= 8 && std::memcmp(d.data(), "icns", 4) == 0) return ReadIcns(d, what);
    return DecodePng(d.data(), d.size(), what);
}

// For each output index, (source index, weight) pairs covering its share of the source
std::vector<std::vector<std::pair<size_t, double>>> Weights(size_t src, size_t dst)
{
    double scale = double(src) / double(dst);
    std::vector<std::vector<std::pair<size_t, double>>> out;
    for (size_t i = 0; i < dst; i++)
    {
        double lo = i * scale, hi = (i + 1) * scale;
        std::vector<std::pair<size_t, double>> ws;
        for (size_t j = size_t(lo); j < hi && j < src; j++)
        {
            double w = std::min(hi, double(j + 1)) - std::max(lo, double(j));
            if (w > 0) ws.push_back({j, w});
        }
        double total = 0;
        for (auto &p : ws) total += p.second;
        for (auto &p : ws) p.second /= total;
        out.push_back(ws);
    }
    return out;
}

// Area-averaged resize of premultiplied rows to size x size (across, then down)
Image ResizeImage(const Image &rows, size_t size)
{
    auto wx = Weights(rows[0].size(), size), wy = Weights(rows.size(), size);
    Image across;
    for (auto &row : rows)
    {
        std::vector<Pixel> out;
        for (auto &ws : wx)
        {
            Pixel s{0, 0, 0, 0};
            for (auto [j, w] : ws) s.r += row[j].r * w, s.g += row[j].g * w, s.b += row[j].b * w, s.a += row[j].a * w;
            out.push_back(s);
        }
        across.push_back(std::move(out));
    }
    Image result;
    for (auto &ws : wy)
    {
        std::vector<Pixel> out;
        for (size_t x = 0; x < size; x++)
        {
            Pixel s{0, 0, 0, 0};
            for (auto [j, w] : ws)
            {
                const Pixel &p = across[j][x];
                s.r += p.r * w, s.g += p.g * w, s.b += p.b * w, s.a += p.a * w;
            }
            out.push_back(s);
        }
        result.push_back(std::move(out));
    }
    return result;
}

// The visible part of the image, center-cropped to a square, premultiplied. A macOS
// icon is a rounded square with a transparent margin and a soft shadow, which would
// be a black frame on iOS (iOS rounds the corners itself).
Image LoadSquare(const fs::path &path, const Log &log)
{
    Image rows = ReadImage(path);
    size_t h = rows.size(), w = h ? rows[0].size() : 0;
    size_t left = 0, top = 0, right = w, bottom = h;
    std::vector<size_t> ys, xs;
    for (size_t y = 0; y < h; y++)
        for (size_t x = 0; x < w; x++)
            if (rows[y][x].a >= 0.5)
            {
                ys.push_back(y);
                break;
            }
    if (!ys.empty())
    {
        size_t minx = w, maxx = 0;
        for (size_t y : ys)
            for (size_t x = 0; x < w; x++)
                if (rows[y][x].a >= 0.5) minx = std::min(minx, x), maxx = std::max(maxx, x);
        left = minx, right = maxx + 1, top = ys.front(), bottom = ys.back() + 1;
    }
    if (right - left != w || bottom - top != h)
        log("  trimmed the transparent margin: " + std::to_string(right - left) + "x" + std::to_string(bottom - top) +
            " of " + std::to_string(w) + "x" + std::to_string(h));
    size_t side = std::min(right - left, bottom - top);
    top += (bottom - top - side) / 2;
    left += (right - left - side) / 2;
    Image out;
    for (size_t y = top; y < top + side; y++)
    {
        std::vector<Pixel> row;
        for (size_t x = left; x < left + side; x++)
        {
            Pixel p = rows[y][x];
            row.push_back({p.r * p.a, p.g * p.a, p.b * p.a, p.a});
        }
        out.push_back(std::move(row));
    }
    return out;
}

// An 8-bit RGB PNG (icons are opaque; transparent parts become black, as on the home screen)
Bytes Png(const Image &img)
{
    size_t h = img.size(), w = img[0].size();
    Bytes rgb;
    rgb.reserve(w * h * 3);
    auto c8 = [](double c)
    {
        return uint8_t(std::max(0.0, std::min(255.0, std::nearbyint(c * 255))));
    };
    for (auto &row : img)
        for (auto &p : row) rgb.push_back(c8(p.r)), rgb.push_back(c8(p.g)), rgb.push_back(c8(p.b));
    size_t len = 0;
    void *p = tdefl_write_image_to_png_file_in_memory_ex(rgb.data(), int(w), int(h), 3, &len, 9, MZ_FALSE);
    if (!p) throw Error("can't write the icon");
    Bytes out(static_cast<uint8_t *>(p), static_cast<uint8_t *>(p) + len);
    mz_free(p);
    return out;
}
}  // namespace

std::map<std::string, Bytes> Make(const fs::path &app, const fs::path &image, const Log &log)
{
    static const std::regex ICON_FILE(R"(AppIcon(\d+(?:\.\d+)?)x\d+(?:\.\d+)?(?:@(\d)x)?(?:~ipad)?\.png)");
    std::vector<std::pair<std::string, size_t>> targets;
    for (auto &entry : fs::directory_iterator(app))
    {
        std::string name = U8Str(entry.path().filename());
        std::smatch m;
        if (std::regex_match(name, m, ICON_FILE))
            targets.push_back(
                {name, size_t(std::lround(std::stod(m[1].str()) * (m[2].matched ? std::stoi(m[2].str()) : 1)))});
    }
    if (targets.empty()) throw Error("the app has no AppIcon*.png files to replace");
    Image img = LoadSquare(image, log);
    if (img.empty()) throw Error(U8Str(image.filename()) + ": the image is empty");
    size_t biggest = 0;
    for (auto &t : targets) biggest = std::max(biggest, t.second);
    if (img.size() < biggest)
        log("  note: the icon is " + std::to_string(img.size()) + "x" + std::to_string(img.size()) + "; " +
            std::to_string(biggest) + "x" + std::to_string(biggest) + " or more looks sharper");
    // Down to the largest size once (the slow part), the rest from that
    std::map<size_t, Image> cache;
    cache[biggest] = img.size() != biggest ? ResizeImage(img, biggest) : img;
    std::map<std::string, Bytes> out;
    for (auto &[name, size] : targets)
    {
        if (!cache.count(size)) cache[size] = ResizeImage(cache[biggest], size);
        out[name] = Png(cache[size]);
    }
    return out;
}

}  // namespace ftl::icon
