#include <windows.h>
#include <cstdio>
#include <cstring>
#include <map>
#include "../client/client.h"
#include "hook.h"

static std::map<std::uint32_t, HookQueryEntry> g_Hooks;

static bool QueryHook(
    Client& client,
    std::uint32_t id,
    HookQueryEntry* entry,
    std::uint64_t* status)
{
    std::uint64_t args[4] = { id, 0, 0, 0 };

    if (!client.Issue(CommandQueryHook, args, status)) {
        return false;
    }
    if (static_cast<std::int32_t>(*status) >= 0) {
        std::memcpy(entry, client.page()->payload, sizeof(*entry));
    }
    return true;
}

bool CmdHookInstall(Client& client, std::uint64_t va, std::uint64_t cookie)
{
    std::uint64_t args[4] = { va, cookie, 0, 0 };
    std::uint64_t result;
    if (!client.Issue(CommandInstall, args, &result)) {
        std::fprintf(stderr, "install timeout\n");
        return false;
    }
    if (static_cast<std::int32_t>(result) < 0) {
        std::fprintf(stderr, "install failed: 0x%08llX\n",
            static_cast<unsigned long long>(result));
        return false;
    }
    std::uint32_t id = static_cast<std::uint32_t>(result);
    g_Hooks[id] = { id, 1, 0, cookie, 0 };
    std::printf("hook_id=%lu\n", static_cast<unsigned long>(id));
    return true;
}

bool CmdHookRemove(Client& client, std::uint32_t id)
{
    std::uint64_t args[4] = { id, 0, 0, 0 };
    std::uint64_t result;
    if (!client.Issue(CommandRemove, args, &result)) {
        std::fprintf(stderr, "remove timeout\n");
        return false;
    }
    if (static_cast<std::int32_t>(result) < 0) {
        std::fprintf(stderr, "remove failed: 0x%08llX\n",
            static_cast<unsigned long long>(result));
        return false;
    }
    g_Hooks.erase(id);
    std::puts("removed");
    return true;
}

bool CmdHookList(Client& client)
{
    constexpr std::size_t kMaxHooks = 64;
    std::uint64_t args[4] = {};
    std::uint64_t count;

    if (!client.Issue(CommandListHooks, args, &count)) {
        std::fprintf(stderr, "list timeout\n");
        return false;
    }
    if (static_cast<std::int32_t>(count) < 0) {
        std::fprintf(stderr, "list failed: 0x%08llX\n",
            static_cast<unsigned long long>(count));
        return false;
    }
    if (count > kMaxHooks ||
        count > sizeof(client.page()->payload) / sizeof(HookQueryEntry)) {
        std::fprintf(stderr, "invalid hook count: %llu\n",
            static_cast<unsigned long long>(count));
        return false;
    }

    g_Hooks.clear();
    std::printf("  %-4s  %-18s  %-18s  %s\n",
        "ID", "GPA", "Cookie", "Hits");
    for (std::size_t index = 0; index < static_cast<std::size_t>(count); ++index) {
        HookQueryEntry entry;
        std::memcpy(
            &entry,
            client.page()->payload + index * sizeof(entry),
            sizeof(entry));
        if (entry.active == 0 || entry.hookId == 0) {
            continue;
        }
        g_Hooks[entry.hookId] = entry;
        std::printf("  %-4lu  0x%016llX  0x%016llX  %lld\n",
            static_cast<unsigned long>(entry.hookId),
            static_cast<unsigned long long>(entry.gpa),
            static_cast<unsigned long long>(entry.cookie),
            static_cast<long long>(entry.hitCount));
    }
    return true;
}

bool CmdHookProbe(Client& client, std::uint64_t value)
{
    std::uint64_t args[4] = { value, 0, 0, 0 };
    std::uint64_t result;

    if (!client.Issue(CommandProbe, args, &result)) {
        std::fprintf(stderr, "probe timeout\n");
        return false;
    }
    std::printf("probe_result=0x%016llX\n",
        static_cast<unsigned long long>(result));
    return true;
}

void CmdHookWatch(Client& client, std::uint32_t id)
{
    if (g_Hooks.find(id) == g_Hooks.end()) {
        std::fprintf(stderr, "hook %lu not cached; run 'hook list' first\n",
            static_cast<unsigned long>(id));
        return;
    }

    std::printf("watching hook %lu (Ctrl+C to stop)\n",
        static_cast<unsigned long>(id));
    for (;;) {
        HookQueryEntry entry{};
        std::uint64_t status;

        if (!QueryHook(client, id, &entry, &status)) {
            std::fprintf(stderr, "\nquery timeout\n");
            return;
        }
        if (static_cast<std::int32_t>(status) < 0) {
            std::fprintf(stderr, "\nquery failed: 0x%08llX\n",
                static_cast<unsigned long long>(status));
            g_Hooks.erase(id);
            return;
        }
        if (entry.active == 0 || entry.hookId != id) {
            std::fprintf(stderr, "\ninvalid hook query response\n");
            return;
        }
        g_Hooks[id] = entry;
        std::printf("\r  hits: %-20lld", static_cast<long long>(entry.hitCount));
        std::fflush(stdout);
        Sleep(500);
    }
}
