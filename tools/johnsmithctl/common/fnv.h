#pragma once
#include <cstdint>

constexpr std::uint32_t kFnvOffset = 0x811c9dc5u;
constexpr std::uint32_t kFnvPrime  = 0x01000193u;

inline std::uint32_t Fnv1a(std::uint32_t seed, const char* tag)
{
    std::uint32_t hash = seed ^ kFnvOffset;
    while (*tag != '\0') {
        hash ^= static_cast<std::uint8_t>(*tag++);
        hash *= kFnvPrime;
    }
    return hash;
}
