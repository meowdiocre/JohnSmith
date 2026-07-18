#pragma once
#include <cstdint>
#include <cstddef>

enum Command : std::uint32_t {
    CommandRegister,
    CommandInstall,
    CommandRemove,
    CommandRead,
    CommandWrite,
    CommandQueryHook,
    CommandListHooks,
    CommandProbe,
    CommandCount
};

constexpr std::uint32_t kPayloadOffset = 256u;

#pragma pack(push, 1)
struct HypercallPage {
    volatile std::uint32_t commandId;
    volatile std::uint32_t sequence;
    volatile std::uint64_t args[4];
    volatile std::uint64_t result;
    volatile std::uint64_t resultSequence;
    std::uint8_t reserved[kPayloadOffset - 56];
    std::uint8_t payload[4096 - kPayloadOffset];
};
#pragma pack(pop)

static_assert(sizeof(HypercallPage) == 4096);
static_assert(offsetof(HypercallPage, payload) == kPayloadOffset);
static_assert(offsetof(HypercallPage, result) == 40);

struct HookQueryEntry {
    std::uint32_t hookId;
    std::uint32_t active;
    std::uint64_t gpa;
    std::uint64_t cookie;
    std::int64_t hitCount;
};

static_assert(sizeof(HookQueryEntry) == 32);
static_assert(offsetof(HookQueryEntry, gpa) == 8);
static_assert(offsetof(HookQueryEntry, hitCount) == 24);

struct Protocol {
    std::uint32_t seed;
    std::uint32_t commandSubleaf;
    std::uint32_t registerSubleaf;
    std::uint32_t commandIds[CommandCount];
};

extern "C" void JsCpuidHypercall(
    std::uint32_t subleaf,
    std::uint64_t rbx,
    std::uint32_t output[4]);
