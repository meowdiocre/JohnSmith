#include <windows.h>
#include <cstring>
#include "client.h"

constexpr DWORD kCommandTimeoutMs = 5000;

Client::Client(const Protocol& protocol) : protocol_(protocol), page_(nullptr)
{
    page_ = static_cast<HypercallPage*>(VirtualAlloc(
        nullptr, sizeof(*page_), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (page_ != nullptr) {
        std::memset(page_, 0, sizeof(*page_));
        if (!VirtualLock(page_, sizeof(*page_))) {
            VirtualFree(page_, 0, MEM_RELEASE);
            page_ = nullptr;
        }
    }
}

Client::~Client()
{
    if (page_ != nullptr) {
        VirtualUnlock(page_, sizeof(*page_));
        VirtualFree(page_, 0, MEM_RELEASE);
    }
}

bool Client::valid() const { return page_ != nullptr; }

bool Client::Register()
{
    std::uint32_t output[4]{};
    ULONGLONG deadline;

    page_->result = ~std::uint64_t{0};
    MemoryBarrier();
    JsCpuidHypercall(
        protocol_.registerSubleaf,
        reinterpret_cast<std::uint64_t>(page_),
        output);

    deadline = GetTickCount64() + kCommandTimeoutMs;
    while (page_->result == ~std::uint64_t{0}) {
        if (GetTickCount64() >= deadline) return false;
        Sleep(1);
        MemoryBarrier();
    }
    return page_->result == 0;
}

bool Client::Issue(Command command, const std::uint64_t args[4], std::uint64_t* result)
{
    std::uint32_t output[4]{};
    std::uint32_t sequence = page_->sequence + 1;
    if (sequence == 0) sequence = 1;
    for (std::size_t i = 0; i < 4; ++i) page_->args[i] = args[i];
    page_->result = 0x103u;
    page_->resultSequence = ~std::uint64_t{0};
    page_->commandId = protocol_.commandIds[command];
    page_->sequence = sequence;
    MemoryBarrier();
    JsCpuidHypercall(protocol_.commandSubleaf, 0, output);

    ULONGLONG deadline = GetTickCount64() + kCommandTimeoutMs;
    while (page_->resultSequence != sequence) {
        if (GetTickCount64() >= deadline) return false;
        Sleep(1);
        MemoryBarrier();
    }
    *result = page_->result;
    return true;
}

HypercallPage* Client::page() { return page_; }
