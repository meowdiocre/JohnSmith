#include <windows.h>
#include <cstdio>
#include <cstring>
#include <vector>
#include "../client/client.h"
#include "mem.h"

bool CmdMemRead(Client& client, std::uint64_t va, std::uint64_t size, const char* outFile)
{
    if (size > sizeof(client.page()->payload)) {
        std::fprintf(stderr, "size exceeds payload (%llu > %llu)\n",
            static_cast<unsigned long long>(size),
            static_cast<unsigned long long>(sizeof(client.page()->payload)));
        return false;
    }
    std::uint64_t args[4] = { va, size, 0, 0 };
    std::uint64_t result;
    if (!client.Issue(CommandRead, args, &result)) {
        std::fprintf(stderr, "read timeout\n");
        return false;
    }
    if (static_cast<std::int32_t>(result) < 0) {
        std::fprintf(stderr, "read failed: 0x%08llX\n", static_cast<unsigned long long>(result));
        return false;
    }
    if (outFile) {
        HANDLE h = CreateFileA(outFile, GENERIC_WRITE, 0, nullptr,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) { std::fprintf(stderr, "cannot open %s\n", outFile); return false; }
        DWORD written;
        WriteFile(h, client.page()->payload, static_cast<DWORD>(size), &written, nullptr);
        CloseHandle(h);
        std::printf("wrote %lu bytes to %s\n", written, outFile);
    } else {
        for (std::uint64_t i = 0; i < size; ++i)
            std::printf("%02X ", client.page()->payload[i]);
        std::putchar('\n');
    }
    return true;
}

bool CmdMemWrite(Client& client, std::uint64_t va, const void* data, std::uint64_t size)
{
    if (size > sizeof(client.page()->payload)) return false;
    std::memcpy(client.page()->payload, data, static_cast<std::size_t>(size));
    std::uint64_t args[4] = { va, size, 0, 0 };
    std::uint64_t result;
    if (!client.Issue(CommandWrite, args, &result)) {
        std::fprintf(stderr, "write timeout\n");
        return false;
    }
    if (static_cast<std::int32_t>(result) < 0) {
        std::fprintf(stderr, "write failed: 0x%08llX\n", static_cast<unsigned long long>(result));
        return false;
    }
    std::puts("write: PASS");
    return true;
}

void CmdMemDump(Client& client, std::uint64_t va, std::uint64_t size)
{
    if (size > sizeof(client.page()->payload)) {
        std::fprintf(stderr, "size exceeds payload\n");
        return;
    }
    std::uint64_t args[4] = { va, size, 0, 0 };
    std::uint64_t result;
    if (!client.Issue(CommandRead, args, &result)) {
        std::fprintf(stderr, "read timeout\n");
        return;
    }
    if (static_cast<std::int32_t>(result) < 0) {
        std::fprintf(stderr, "read failed: 0x%08llX\n",
            static_cast<unsigned long long>(result));
        return;
    }
    for (std::uint64_t off = 0; off < size; off += 16) {
        std::printf("  0x%016llX  ", static_cast<unsigned long long>(va + off));
        for (std::uint64_t k = 0; k < 16 && off + k < size; ++k)
            std::printf("%02X ", client.page()->payload[off + k]);
        for (std::uint64_t k2 = (std::uint64_t)0; k2 < 16 && (off + 16) < (size + 16); ++k2)
            if (off + k2 >= size) std::printf("   ");
        std::printf(" |");
        for (std::uint64_t j = 0; j < 16 && off + j < size; ++j) {
            std::uint8_t c = client.page()->payload[off + j];
            std::putchar(c >= 32 && c < 127 ? static_cast<char>(c) : '.');
        }
        std::puts("|");
    }
}

void CmdMemScan(Client& client, std::uint64_t va, std::uint64_t size, const char* pattern)
{
    std::vector<std::uint8_t> patBytes;
    const std::size_t patLen = std::strlen(pattern);
    if (patLen == 0 || (patLen & 1)) { std::fprintf(stderr, "invalid pattern\n"); return; }
    patBytes.resize(patLen / 2);
    for (std::size_t i = 0; i < patBytes.size(); ++i) {
        auto nib = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        int h = nib(pattern[i * 2]), l = nib(pattern[i * 2 + 1]);
        if (h < 0 || l < 0) { std::fprintf(stderr, "invalid hex in pattern\n"); return; }
        patBytes[i] = static_cast<std::uint8_t>((h << 4) | l);
    }

    if (size > sizeof(client.page()->payload)) {
        std::fprintf(stderr, "size exceeds payload\n");
        return;
    }
    std::uint64_t args[4] = { va, size, 0, 0 };
    std::uint64_t result;
    if (!client.Issue(CommandRead, args, &result)) {
        std::fprintf(stderr, "read timeout\n");
        return;
    }
    if (static_cast<std::int32_t>(result) < 0) {
        std::fprintf(stderr, "read failed: 0x%08llX\n",
            static_cast<unsigned long long>(result));
        return;
    }

    std::vector<std::uint64_t> matches;
    for (std::uint64_t i = 0; i + patBytes.size() <= size; ++i) {
        if (std::memcmp(&client.page()->payload[i], patBytes.data(), patBytes.size()) == 0)
            matches.push_back(va + i);
    }
    if (matches.empty())
        std::puts("no matches");
    else {
        std::printf("%llu match%s at:\n",
            static_cast<unsigned long long>(matches.size()),
            matches.size() == 1 ? "" : "es");
        for (auto m : matches)
            std::printf("  0x%016llX\n", static_cast<unsigned long long>(m));
    }
}
