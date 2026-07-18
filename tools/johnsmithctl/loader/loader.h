#pragma once
#include <cstdint>

struct DriverConfig {
    const wchar_t* service;
    std::uint32_t  provider;
};

struct LoadResult {
    bool          loaded;
    std::uint32_t seed;
    std::uint32_t dseOriginal;
    char          error[256];
};

LoadResult     DriverLoad(const DriverConfig& cfg, std::uint32_t seed);
bool           DriverUnload(const DriverConfig& cfg);
std::uint32_t  DriverStatus(const DriverConfig& cfg);
