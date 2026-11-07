// Property lists: reads XML and binary plists, writes XML (keys sorted, tab indents, the
// format Apple's tools and Python's plistlib use).
#pragma once
#include <map>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "util.h"

namespace ftl::plist
{

struct Value;
using Array = std::vector<Value>;
using Dict = std::map<std::string, Value>;  // sorted, as plistlib writes them

struct Date
{
    std::string iso;
};  // "2020-01-02T03:04:05Z"
struct Data
{
    Bytes bytes;
};

struct Value
{
    std::variant<bool, int64_t, double, std::string, Date, Data, Array, Dict> v;
    Value() : v(std::string()) {}
    Value(bool x) : v(x) {}
    Value(int x) : v(int64_t(x)) {}
    Value(int64_t x) : v(x) {}
    Value(double x) : v(x) {}
    Value(std::string x) : v(std::move(x)) {}
    Value(const char *s) : v(std::string(s)) {}
    Value(Date x) : v(std::move(x)) {}
    Value(Data x) : v(std::move(x)) {}
    Value(Array x) : v(std::move(x)) {}
    Value(Dict x) : v(std::move(x)) {}

    template <class T> bool Is() const
    {
        return std::holds_alternative<T>(v);
    }
    template <class T> T &As()
    {
        return std::get<T>(v);
    }
    template <class T> const T &As() const
    {
        return std::get<T>(v);
    }
};

Value Parse(const Bytes &data);  // XML or binary
std::string ToXml(const Value &root);

}  // namespace ftl::plist
