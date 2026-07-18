#pragma once
#include <cstdint>
#include <cstdlib>
#include <cerrno>
#include <cstring>

inline bool ParseU64(const char* text, std::uint64_t* value)
{
    char* end = nullptr;
    if (text == nullptr || value == nullptr || *text == '\0' || *text == '-')
        return false;
    errno = 0;
    unsigned long long parsed = std::strtoull(text, &end, 0);
    if (errno == ERANGE || end == text || *end != '\0')
        return false;
    *value = static_cast<std::uint64_t>(parsed);
    return true;
}
