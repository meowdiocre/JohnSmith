#pragma once
#include <cstdint>

struct DseCtx {
    std::uint32_t provider;
    std::uint32_t original;
    char         error[256];
};

bool DseDisable(DseCtx* ctx, std::uint32_t providerId);
bool DseRestore(DseCtx* ctx);
void DseListProviders();
std::uint32_t DseAutoSelect();
