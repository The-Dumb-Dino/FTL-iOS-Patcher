#include "plist.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <ctime>

namespace ftl::plist
{

// ---- base64 ----

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string B64Encode(const uint8_t *p, size_t n)
{
    std::string out;
    for (size_t i = 0; i < n; i += 3)
    {
        uint32_t v = uint32_t(p[i]) << 16 | (i + 1 < n ? uint32_t(p[i + 1]) << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
        out += B64[v >> 18 & 63];
        out += B64[v >> 12 & 63];
        out += i + 1 < n ? B64[v >> 6 & 63] : '=';
        out += i + 2 < n ? B64[v & 63] : '=';
    }
    return out;
}

static Bytes B64Decode(const std::string &s)
{
    Bytes out;
    uint32_t acc = 0;
    int bits = 0;
    for (char c : s)
    {
        const char *p = c ? std::strchr(B64, c) : nullptr;
        if (!p) continue;  // whitespace, padding
        acc = acc << 6 | uint32_t(p - B64);
        bits += 6;
        if (bits >= 8)
        {
            bits -= 8;
            out.push_back(uint8_t(acc >> bits));
        }
    }
    return out;
}

// ---- XML reading ----

namespace
{
struct XmlReader
{
    const std::string &s;
    size_t pos = 0;

    [[noreturn]] void Fail(const std::string &what)
    {
        throw Error("Info.plist: " + what);
    }

    void SkipMisc()
    {
        for (;;)
        {
            while (pos < s.size() && std::isspace(uint8_t(s[pos]))) pos++;
            if (s.compare(pos, 4, "<!--") == 0)
            {
                size_t e = s.find("-->", pos);
                if (e == std::string::npos) Fail("unterminated comment");
                pos = e + 3;
            }
            else if (s.compare(pos, 2, "<?") == 0 || s.compare(pos, 2, "<!") == 0)
            {
                size_t e = s.find('>', pos);
                if (e == std::string::npos) Fail("broken XML");
                pos = e + 1;
            }
            else
            {
                return;
            }
        }
    }

    // Reads "<name attrs>" or "<name/>"; returns the name, sets `empty`
    std::string OpenTag(bool &empty)
    {
        SkipMisc();
        if (pos >= s.size() || s[pos] != '<' || s.compare(pos, 2, "</") == 0) Fail("expected an element");
        size_t e = s.find('>', pos);
        if (e == std::string::npos) Fail("broken XML");
        std::string inner = s.substr(pos + 1, e - pos - 1);
        pos = e + 1;
        empty = !inner.empty() && inner.back() == '/';
        if (empty) inner.pop_back();
        size_t sp = inner.find_first_of(" \t\r\n");
        return inner.substr(0, sp);
    }

    bool AtClose()
    {
        SkipMisc();
        return s.compare(pos, 2, "</") == 0;
    }

    void CloseTag(const std::string &name)
    {
        SkipMisc();
        std::string want = "</" + name;
        if (s.compare(pos, want.size(), want) != 0) Fail("expected </" + name + ">");
        size_t e = s.find('>', pos);
        pos = e + 1;
    }

    std::string Text(const std::string &name)
    {
        size_t e = s.find("</" + name, pos);
        if (e == std::string::npos) Fail("unterminated <" + name + ">");
        std::string raw = s.substr(pos, e - pos);
        pos = e;
        CloseTag(name);
        std::string out;
        for (size_t i = 0; i < raw.size(); i++)
        {
            if (raw[i] != '&')
            {
                out += raw[i];
                continue;
            }
            size_t semi = raw.find(';', i);
            if (semi == std::string::npos) Fail("broken entity");
            std::string ent = raw.substr(i + 1, semi - i - 1);
            if (ent == "lt")
                out += '<';
            else if (ent == "gt")
                out += '>';
            else if (ent == "amp")
                out += '&';
            else if (ent == "quot")
                out += '"';
            else if (ent == "apos")
                out += '\'';
            else if (!ent.empty() && ent[0] == '#')
            {
                uint32_t cp = ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X')
                                  ? uint32_t(std::strtoul(ent.c_str() + 2, nullptr, 16))
                                  : uint32_t(std::strtoul(ent.c_str() + 1, nullptr, 10));
                if (cp < 0x80)
                    out += char(cp);
                else if (cp < 0x800)
                {
                    out += char(0xC0 | cp >> 6);
                    out += char(0x80 | (cp & 63));
                }
                else if (cp < 0x10000)
                {
                    out += char(0xE0 | cp >> 12);
                    out += char(0x80 | (cp >> 6 & 63));
                    out += char(0x80 | (cp & 63));
                }
                else
                {
                    out += char(0xF0 | cp >> 18);
                    out += char(0x80 | (cp >> 12 & 63));
                    out += char(0x80 | (cp >> 6 & 63));
                    out += char(0x80 | (cp & 63));
                }
            }
            else
                Fail("unknown entity &" + ent + ";");
            i = semi;
        }
        return out;
    }

    Value ParseValue()
    {
        bool empty;
        std::string name = OpenTag(empty);
        if (name == "true" || name == "false")
        {
            if (!empty) CloseTag(name);
            return Value(name == "true");
        }
        if (name == "dict")
        {
            Dict d;
            if (empty) return d;
            while (!AtClose())
            {
                bool e2;
                if (OpenTag(e2) != "key") Fail("expected <key>");
                std::string k = e2 ? "" : Text("key");
                d[k] = ParseValue();
            }
            CloseTag("dict");
            return d;
        }
        if (name == "array")
        {
            Array a;
            if (empty) return a;
            while (!AtClose()) a.push_back(ParseValue());
            CloseTag("array");
            return a;
        }
        std::string t = empty ? "" : Text(name);
        if (name == "string") return t;
        if (name == "integer")
        {
            const char *c = t.c_str();
            while (std::isspace(uint8_t(*c))) c++;
            bool hex = c[0] == '0' && (c[1] == 'x' || c[1] == 'X');
            return Value(int64_t(hex ? std::strtoull(c + 2, nullptr, 16) : std::strtoll(c, nullptr, 10)));
        }
        if (name == "real") return Value(std::strtod(t.c_str(), nullptr));
        if (name == "date") return Date{t};
        if (name == "data") return Data{B64Decode(t)};
        Fail("unknown element <" + name + ">");
    }

    Value Root()
    {
        bool empty;
        if (OpenTag(empty) != "plist") Fail("no <plist>");
        Value v = ParseValue();
        CloseTag("plist");
        return v;
    }
};

// ---- binary reading ----

struct BinReader
{
    const Bytes &d;
    int refSize = 0, offSize = 0;
    uint64_t nobjects = 0, table = 0;
    int depth = 0;

    [[noreturn]] void Fail()
    {
        throw Error("Info.plist: broken binary plist");
    }

    uint64_t Be(size_t at, int n)
    {
        if (at + n > d.size()) Fail();
        uint64_t v = 0;
        for (int i = 0; i < n; i++) v = v << 8 | d[at + i];
        return v;
    }

    uint64_t OffsetOf(uint64_t ref)
    {
        if (ref >= nobjects) Fail();
        return Be(size_t(table + ref * offSize), offSize);
    }

    // The count in the marker's low nibble, or an int object after it
    uint64_t Count(size_t &at)
    {
        uint8_t m = d[at++];
        if ((m & 15) != 15) return m & 15;
        if (at >= d.size() || (d[at] & 0xF0) != 0x10) Fail();
        int n = 1 << (d[at] & 15);
        uint64_t c = Be(at + 1, n);
        at += 1 + n;
        return c;
    }

    Value Object(uint64_t ref)
    {
        if (++depth > 64) Fail();
        size_t at = size_t(OffsetOf(ref));
        if (at >= d.size()) Fail();
        uint8_t m = d[at];
        Value out;
        switch (m >> 4)
        {
            case 0x0:
                if (m == 8 || m == 9)
                    out = Value(m == 9);
                else
                    Fail();
                break;
            case 0x1:
            {
                int n = 1 << (m & 15);
                uint64_t v = Be(at + 1, n);
                out = Value(int64_t(n == 8 ? v : v));  // 8 bytes: signed two's complement
                break;
            }
            case 0x2:
            {
                int n = 1 << (m & 15);
                uint64_t v = Be(at + 1, n);
                if (n == 4)
                {
                    uint32_t u = uint32_t(v);
                    float f;
                    std::memcpy(&f, &u, 4);
                    out = Value(double(f));
                }
                else if (n == 8)
                {
                    double f;
                    std::memcpy(&f, &v, 8);
                    out = Value(f);
                }
                else
                    Fail();
                break;
            }
            case 0x3:
            {
                uint64_t v = Be(at + 1, 8);
                double secs;
                std::memcpy(&secs, &v, 8);
                std::time_t t = std::time_t(std::floor(secs)) + 978307200;  // since 2001-01-01
                std::tm tm{};
#ifdef _WIN32
                gmtime_s(&tm, &t);
#else
                gmtime_r(&t, &tm);
#endif
                char buf[32];
                std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
                out = Date{buf};
                break;
            }
            case 0x4:
            {
                uint64_t n = Count(at);
                if (at + n > d.size()) Fail();
                out = Data{Bytes(d.begin() + at, d.begin() + at + n)};
                break;
            }
            case 0x5:
            {
                uint64_t n = Count(at);
                if (at + n > d.size()) Fail();
                std::string s;
                for (uint64_t i = 0; i < n; i++)
                {  // ASCII (Latin-1 in practice) to UTF-8
                    uint8_t c = d[at + i];
                    if (c < 0x80)
                        s += char(c);
                    else
                    {
                        s += char(0xC0 | c >> 6);
                        s += char(0x80 | (c & 63));
                    }
                }
                out = s;
                break;
            }
            case 0x6:
            {
                uint64_t n = Count(at);
                if (at + n * 2 > d.size()) Fail();
                std::string s;
                for (uint64_t i = 0; i < n; i++)
                {
                    uint32_t cp = d[at + 2 * i] << 8 | d[at + 2 * i + 1];
                    if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < n)
                    {
                        uint32_t lo = d[at + 2 * i + 2] << 8 | d[at + 2 * i + 3];
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        i++;
                    }
                    if (cp < 0x80)
                        s += char(cp);
                    else if (cp < 0x800)
                    {
                        s += char(0xC0 | cp >> 6);
                        s += char(0x80 | (cp & 63));
                    }
                    else if (cp < 0x10000)
                    {
                        s += char(0xE0 | cp >> 12);
                        s += char(0x80 | (cp >> 6 & 63));
                        s += char(0x80 | (cp & 63));
                    }
                    else
                    {
                        s += char(0xF0 | cp >> 18);
                        s += char(0x80 | (cp >> 12 & 63));
                        s += char(0x80 | (cp >> 6 & 63));
                        s += char(0x80 | (cp & 63));
                    }
                }
                out = s;
                break;
            }
            case 0xA:
            {
                uint64_t n = Count(at);
                Array a;
                for (uint64_t i = 0; i < n; i++) a.push_back(Object(Be(at + i * refSize, refSize)));
                out = a;
                break;
            }
            case 0xD:
            {
                uint64_t n = Count(at);
                Dict dict;
                for (uint64_t i = 0; i < n; i++)
                {
                    Value k = Object(Be(at + i * refSize, refSize));
                    if (!k.Is<std::string>()) Fail();
                    dict[k.As<std::string>()] = Object(Be(at + (n + i) * refSize, refSize));
                }
                out = dict;
                break;
            }
            default: Fail();
        }
        depth--;
        return out;
    }

    Value Root()
    {
        if (d.size() < 40) Fail();
        size_t tr = d.size() - 32;
        offSize = d[tr + 6];
        refSize = d[tr + 7];
        nobjects = Be(tr + 8, 8);
        uint64_t top = Be(tr + 16, 8);
        table = Be(tr + 24, 8);
        if (offSize < 1 || offSize > 8 || refSize < 1 || refSize > 8 || table >= d.size()) Fail();
        return Object(top);
    }
};
}  // namespace

Value Parse(const Bytes &data)
{
    if (data.size() >= 8 && std::memcmp(data.data(), "bplist00", 8) == 0) return BinReader{data}.Root();
    std::string s(data.begin(), data.end());
    return XmlReader{s}.Root();
}

// ---- XML writing, as plistlib does ----

static std::string Escape(const std::string &s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); i++)
    {
        char c = s[i];
        if (c == '\r')
        {  // plistlib: \r\n and \r become \n
            out += '\n';
            if (i + 1 < s.size() && s[i + 1] == '\n') i++;
        }
        else if (c == '&')
            out += "&amp;";
        else if (c == '<')
            out += "&lt;";
        else if (c == '>')
            out += "&gt;";
        else
            out += c;
    }
    return out;
}

// A real number: the shortest digits that read back the same, fixed for exponents -4 to 15
static std::string Repr(double v)
{
    if (std::isnan(v)) return "nan";
    if (std::isinf(v)) return v < 0 ? "-inf" : "inf";
    char buf[64];
    int prec = 1;
    for (; prec <= 17; prec++)
    {
        std::snprintf(buf, sizeof buf, "%.*e", prec - 1, v);
        if (std::strtod(buf, nullptr) == v) break;
    }
    // buf: [-]d[.ddd]e[+-]XX
    std::string s(buf);
    bool neg = s[0] == '-';
    if (neg) s.erase(0, 1);
    size_t e = s.find('e');
    int exp = std::atoi(s.c_str() + e + 1);
    std::string digits;
    for (size_t i = 0; i < e; i++)
        if (s[i] != '.') digits += s[i];
    while (digits.size() > 1 && digits.back() == '0') digits.pop_back();
    std::string out;
    if (exp >= -4 && exp < 16)
    {  // repr's 'r' format: fixed for exponents -4 to 15
        if (exp < 0)
        {
            out = "0." + std::string(size_t(-exp - 1), '0') + digits;
        }
        else if (int(digits.size()) <= exp + 1)
        {
            out = digits + std::string(size_t(exp + 1 - digits.size()), '0') + ".0";
        }
        else
        {
            out = digits.substr(0, size_t(exp + 1)) + "." + digits.substr(size_t(exp + 1));
        }
    }
    else
    {
        out = digits.substr(0, 1);
        if (digits.size() > 1) out += "." + digits.substr(1);
        char eb[16];
        std::snprintf(eb, sizeof eb, "e%c%02d", exp < 0 ? '-' : '+', exp < 0 ? -exp : exp);
        out += eb;
    }
    return (neg ? "-" : "") + out;
}

static void Write(std::string &out, const Value &v, int level)
{
    auto line = [&](const std::string &text, int lv)
    {
        out.append(size_t(lv), '\t');
        out += text;
        out += '\n';
    };
    if (v.Is<bool>())
        line(v.As<bool>() ? "<true/>" : "<false/>", level);
    else if (v.Is<int64_t>())
        line("<integer>" + std::to_string(v.As<int64_t>()) + "</integer>", level);
    else if (v.Is<double>())
        line("<real>" + Repr(v.As<double>()) + "</real>", level);
    else if (v.Is<std::string>())
        line("<string>" + Escape(v.As<std::string>()) + "</string>", level);
    else if (v.Is<Date>())
        line("<date>" + v.As<Date>().iso + "</date>", level);
    else if (v.Is<Data>())
    {
        line("<data>", level);
        const Bytes &b = v.As<Data>().bytes;
        int width = std::max(16, 76 - 8 * level);
        size_t chunk = size_t(width / 4 * 3);
        for (size_t i = 0; i < b.size(); i += chunk) line(B64Encode(&b[i], std::min(chunk, b.size() - i)), level);
        line("</data>", level);
    }
    else if (v.Is<Array>())
    {
        const Array &a = v.As<Array>();
        if (a.empty()) return line("<array/>", level);
        line("<array>", level);
        for (auto &x : a) Write(out, x, level + 1);
        line("</array>", level);
    }
    else
    {
        const Dict &d = v.As<Dict>();
        if (d.empty()) return line("<dict/>", level);
        line("<dict>", level);
        for (auto &[k, x] : d)
        {
            line("<key>" + Escape(k) + "</key>", level + 1);
            Write(out, x, level + 1);
        }
        line("</dict>", level);
    }
}

std::string ToXml(const Value &root)
{
    std::string out =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        "<plist version=\"1.0\">\n";
    Write(out, root, 0);
    out += "</plist>\n";
    return out;
}

}  // namespace ftl::plist
