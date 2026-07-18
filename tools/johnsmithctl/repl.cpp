#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#pragma warning(disable: 4996)  /* strtok */
#include "repl.h"
#include "common/parse.h"
#include "common/types.h"
#include "commands/hook.h"
#include "commands/mem.h"
#include "commands/resolve.h"

class AffinityGuard {
public:
    explicit AffinityGuard(std::uint32_t cpu) : prev_(0) {
        if (cpu >= sizeof(DWORD_PTR) * 8) return;
        prev_ = SetThreadAffinityMask(GetCurrentThread(), static_cast<DWORD_PTR>(1) << cpu);
        if (prev_ != 0) SwitchToThread();
    }
    ~AffinityGuard() { if (prev_ != 0) SetThreadAffinityMask(GetCurrentThread(), prev_); }
    bool valid() const { return prev_ != 0; }
private:
    DWORD_PTR prev_;
};

static void PrintHelp()
{
    std::puts(
        "  hook install <va> [cookie]    Install EPT hook at kernel VA\n"
        "  hook remove <id>              Remove hook by ID\n"
        "  hook list                     List all active hooks\n"
        "  hook probe <value>            Invoke cold probe at PASSIVE_LEVEL\n"
        "  hook watch <id>               Live hit counter\n"
        "  mem read <va> <size> [--out]  Read memory\n"
        "  mem write <va> <hex>          Write memory\n"
        "  mem dump <va> <size>          Hexdump + ASCII\n"
        "  mem scan <va> <size> <pat>    Find byte pattern\n"
        "  provider list                 Show DSE providers\n"
        "  resolve <module> <export>     Resolve live kernel export\n"
        "  status                        Driver state\n"
        "  help                          This message\n"
        "  exit                          Leave driver running\n");
}

static bool ParseHexBytes(const char* text, std::vector<std::uint8_t>* bytes)
{
    std::size_t len = text ? std::strlen(text) : 0;
    if (len == 0 || (len & 1) || len / 2 > 4096) return false;
    bytes->resize(len / 2);
    for (std::size_t i = 0; i < bytes->size(); ++i) {
        auto nib = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        int h = nib(text[i*2]), l = nib(text[i*2+1]);
        if (h < 0 || l < 0) return false;
        (*bytes)[i] = static_cast<std::uint8_t>((h << 4) | l);
    }
    return true;
}

int RunRepl(Client& client, const Protocol& /*protocol*/, std::uint32_t cpu)
{
    AffinityGuard affinity(cpu);
    if (!affinity.valid()) {
        std::fprintf(stderr, "affinity failed\n");
        return 1;
    }
    if (!client.Register()) {
        std::fprintf(stderr, "registration failed\n");
        return 1;
    }
    std::printf("registered on cpu %lu\n\n", static_cast<unsigned long>(cpu));

    char line[512];
    for (;;) {
        std::fputs("johnsmithctl> ", stdout);
        if (!std::fgets(line, sizeof(line), stdin)) break;
        std::size_t len = std::strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) line[--len] = 0;
        if (len == 0) continue;

        std::vector<char*> tokens;
        char* tok = std::strtok(line, " ");
        while (tok) { tokens.push_back(tok); tok = std::strtok(nullptr, " "); }
        if (tokens.empty()) continue;

        const char* cmd = tokens[0];

        if (std::strcmp(cmd, "exit") == 0 || std::strcmp(cmd, "quit") == 0) {
            break;
        }
        if (std::strcmp(cmd, "help") == 0) {
            PrintHelp();
        }
        else if (std::strcmp(cmd, "status") == 0) {
            std::printf("  backend: (query via hypercall)\n  cpu: %lu\n  registered: yes\n",
                static_cast<unsigned long>(cpu));
        }
        else if (std::strcmp(cmd, "provider") == 0 && tokens.size() >= 2 &&
                 std::strcmp(tokens[1], "list") == 0) {
            DseListProviders();
        }
        else if (std::strcmp(cmd, "resolve") == 0) {
            if (tokens.size() != 3) {
                std::fprintf(stderr, "usage: resolve <module> <export>\n");
                continue;
            }
            CmdResolve(client, tokens[1], tokens[2]);
        }
        else if (std::strcmp(cmd, "hook") == 0 && tokens.size() >= 2) {
            if (std::strcmp(tokens[1], "install") == 0) {
                std::uint64_t va, cookie = 0;
                if (tokens.size() < 3) { std::fprintf(stderr, "missing va\n"); continue; }
                if (!ParseU64(tokens[2], &va)) { std::fprintf(stderr, "invalid va\n"); continue; }
                if (tokens.size() >= 4 && !ParseU64(tokens[3], &cookie)) {
                    std::fprintf(stderr, "invalid cookie\n"); continue;
                }
                CmdHookInstall(client, va, cookie);
            }
            else if (std::strcmp(tokens[1], "remove") == 0) {
                std::uint64_t id;
                if (tokens.size() < 3) { std::fprintf(stderr, "missing id\n"); continue; }
                if (!ParseU64(tokens[2], &id) || id > UINT32_MAX) {
                    std::fprintf(stderr, "invalid id\n"); continue;
                }
                CmdHookRemove(client, static_cast<std::uint32_t>(id));
            }
            else if (std::strcmp(tokens[1], "list") == 0) {
                CmdHookList(client);
            }
            else if (std::strcmp(tokens[1], "probe") == 0) {
                std::uint64_t value;
                if (tokens.size() != 3 || !ParseU64(tokens[2], &value)) {
                    std::fprintf(stderr, "usage: hook probe <value>\n");
                    continue;
                }
                CmdHookProbe(client, value);
            }
            else if (std::strcmp(tokens[1], "watch") == 0) {
                std::uint64_t id;
                if (tokens.size() < 3) { std::fprintf(stderr, "missing id\n"); continue; }
                if (!ParseU64(tokens[2], &id) || id > UINT32_MAX) {
                    std::fprintf(stderr, "invalid id\n"); continue;
                }
                CmdHookWatch(client, static_cast<std::uint32_t>(id));
            }
            else { std::fprintf(stderr, "unknown hook command\n"); }
        }
        else if (std::strcmp(cmd, "mem") == 0 && tokens.size() >= 4) {
            std::uint64_t va, size;
            if (!ParseU64(tokens[2], &va) || !ParseU64(tokens[3], &size)) {
                std::fprintf(stderr, "invalid address/size\n"); continue;
            }
            if (std::strcmp(tokens[1], "read") == 0) {
                const char* outFile = nullptr;
                if (tokens.size() >= 5 && std::strcmp(tokens[4], "--out") == 0 && tokens.size() >= 6)
                    outFile = tokens[5];
                CmdMemRead(client, va, size, outFile);
            }
            else if (std::strcmp(tokens[1], "dump") == 0) {
                CmdMemDump(client, va, size);
            }
            else if (std::strcmp(tokens[1], "scan") == 0 && tokens.size() >= 5) {
                CmdMemScan(client, va, size, tokens[4]);
            }
            else if (std::strcmp(tokens[1], "write") == 0 && tokens.size() >= 5) {
                std::vector<std::uint8_t> bytes;
                if (!ParseHexBytes(tokens[4], &bytes)) { std::fprintf(stderr, "invalid hex\n"); continue; }
                CmdMemWrite(client, va, bytes.data(), bytes.size());
            }
            else { std::fprintf(stderr, "unknown mem command\n"); }
        }
        else {
            std::fprintf(stderr, "unknown command. type 'help'\n");
        }
    }
    std::puts("session closed, driver still loaded.");
    return 0;
}
