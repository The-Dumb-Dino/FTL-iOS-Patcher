#pragma once
#include <array>
#include <cstddef>
#include <cstdint>

namespace ftl
{
std::array<uint8_t, 32> Sha256(const uint8_t *data, size_t len);
}
