// The Mach-O work the patcher needs, on a thin 64-bit arm64 executable
// (no Apple tools needed).
#pragma once
#include <array>
#include <map>
#include <optional>
#include <string>
#include <tuple>

#include "util.h"

namespace ftl::macho
{

constexpr uint32_t IOS_PLATFORM = 2;
constexpr uint32_t IOS_SIMULATOR_PLATFORM = 7;

struct Segment
{
    size_t cmd;  // offset of its load command
    uint64_t vmaddr, vmsize, fileoff, filesize;
};

struct NewSegment
{
    std::string name;
    uint64_t size;
    std::string prot;  // like "rx"
};

Bytes Thin(const Bytes &data);
void CheckDecrypted(const Bytes &data);
std::optional<std::array<uint8_t, 16>> Uuid(const Bytes &data);
std::map<std::string, Segment> Segments(const Bytes &data);
void SetBuildVersion(Bytes &data, uint32_t platform, const std::string &minos, const std::string &sdk);
void AddDylib(Bytes &data, const std::string &name);
void RemoveSignature(Bytes &data);
std::map<std::string, std::tuple<uint64_t, uint64_t, uint64_t>> AddSegments(Bytes &data,
                                                                            const std::vector<NewSegment> &segs);
void AdhocSign(Bytes &data, const std::string &identifier);

}  // namespace ftl::macho
