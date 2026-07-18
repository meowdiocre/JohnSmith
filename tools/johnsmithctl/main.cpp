#include <windows.h>
#include <bcrypt.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "common/fnv.h"
#include "common/parse.h"
#include "common/types.h"
#include "client/client.h"
#include "loader/loader.h"
#include "loader/dse.h"
#include "commands/hook.h"
#include "commands/mem.h"
#include "commands/resolve.h"
#include "repl.h"

#pragma comment(lib, "bcrypt.lib")

static Protocol DeriveProtocol(std::uint32_t seed)
{
    Protocol p{};
    const char* tags[CommandCount] = {
        "reg", "inst", "rem", "read", "writ", "quer", "list", "prob"
    };
    p.seed = seed;
    p.commandSubleaf = Fnv1a(seed, "sub");
    p.registerSubleaf = Fnv1a(seed, "srg");
    for (std::uint32_t i = 0; i < CommandCount; ++i)
        p.commandIds[i] = Fnv1a(seed, tags[i]);
    return p;
}

static std::uint32_t GenerateSeed()
{
    std::uint32_t seed = 0;
    BCryptGenRandom(nullptr, reinterpret_cast<BYTE*>(&seed),
        sizeof(seed), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return seed == 0 ? 1 : seed;
}

static void PrintProtocol(const Protocol& p)
{
    std::printf("seed=0x%08X command_subleaf=0x%08X register_subleaf=0x%08X\n",
        p.seed, p.commandSubleaf, p.registerSubleaf);
    std::printf("register=0x%08X install=0x%08X remove=0x%08X read=0x%08X write=0x%08X query=0x%08X list=0x%08X probe=0x%08X\n",
        p.commandIds[CommandRegister], p.commandIds[CommandInstall],
        p.commandIds[CommandRemove], p.commandIds[CommandRead],
        p.commandIds[CommandWrite], p.commandIds[CommandQueryHook],
        p.commandIds[CommandListHooks], p.commandIds[CommandProbe]);
}

static int SelfTest()
{
    Protocol p = DeriveProtocol(0x12345678u);
    bool pass =
        p.commandSubleaf == 0x226db25du &&
        p.registerSubleaf == 0x1d5c615du &&
        p.commandIds[CommandRegister] == 0x8a13feddu &&
        p.commandIds[CommandInstall] == 0xc6e52d51u &&
        p.commandIds[CommandRemove] == 0x8013ef1fu &&
        p.commandIds[CommandRead] == 0xfa6ac69du &&
        p.commandIds[CommandWrite] == 0x950016b9u &&
        p.commandIds[CommandQueryHook] == 0xa6701220u &&
        p.commandIds[CommandListHooks] == 0xfc225b19u &&
        p.commandIds[CommandProbe] == 0x407f55b6u &&
        ResolveSelfTest();
    std::puts(pass ? "selftest: PASS" : "selftest: FAIL");
    return pass ? 0 : 1;
}

static void PrintHelp()
{
    std::puts(
        "johnsmithctl selftest\n"
        "johnsmithctl derive <seed>\n"
        "johnsmithctl list-providers\n"
        "johnsmithctl resolve <module> <export> [--seed <hex>] [--cpu <n>]\n"
        "johnsmithctl start [--seed <hex>] [--cpu <n>]\n"
        "                 [--provider <id>]\n"
        "                 [-- <command> [args...]]\n"
        "johnsmithctl stop\n"
        "johnsmithctl --help");
}

int main(int argc, char** argv)
{
    if (argc >= 2 && (std::strcmp(argv[1], "--help") == 0 ||
                      std::strcmp(argv[1], "-h") == 0 ||
                      std::strcmp(argv[1], "help") == 0)) {
        PrintHelp();
        return 0;
    }

    if (argc == 2 && std::strcmp(argv[1], "selftest") == 0)
        return SelfTest();

    if (argc == 3 && std::strcmp(argv[1], "derive") == 0) {
        std::uint64_t seed64;
        if (!ParseU64(argv[2], &seed64) || seed64 > UINT32_MAX) {
            std::fprintf(stderr, "invalid seed\n"); return 2;
        }
        PrintProtocol(DeriveProtocol(static_cast<std::uint32_t>(seed64)));
        return 0;
    }

    if (argc >= 2 && std::strcmp(argv[1], "list-providers") == 0) {
        DseListProviders();
        return 0;
    }

    if (argc >= 4 && std::strcmp(argv[1], "resolve") == 0) {
        std::uint32_t seed = 0;
        std::uint32_t cpu = static_cast<std::uint32_t>(
            GetCurrentProcessorNumber());
        bool seedProvided = false;
        DriverConfig cfg{ L"JohnSmith", 0xFFFF };

        for (int index = 4; index < argc; ++index) {
            if (std::strcmp(argv[index], "--seed") == 0 &&
                index + 1 < argc) {
                std::uint64_t value;
                if (!ParseU64(argv[++index], &value) ||
                    value == 0 || value > UINT32_MAX) {
                    std::fprintf(stderr, "invalid seed\n");
                    return 2;
                }
                seed = static_cast<std::uint32_t>(value);
                seedProvided = true;
            }
            else if (std::strcmp(argv[index], "--cpu") == 0 &&
                     index + 1 < argc) {
                std::uint64_t value;
                if (!ParseU64(argv[++index], &value) || value > UINT32_MAX) {
                    std::fprintf(stderr, "invalid cpu\n");
                    return 2;
                }
                cpu = static_cast<std::uint32_t>(value);
            }
            else {
                std::fprintf(stderr, "unknown argument: %s\n", argv[index]);
                return 2;
            }
        }

        if (!seedProvided) {
            if (DriverStatus(cfg) == SERVICE_RUNNING) {
                std::fprintf(stderr,
                    "driver already running; pass its original --seed\n");
                return 1;
            }
            seed = GenerateSeed();
        }

        LoadResult load = DriverLoad(cfg, seed);
        bool alreadyRunning = std::strcmp(load.error, "already running") == 0;
        if (!load.loaded || (alreadyRunning && !seedProvided)) {
            std::fprintf(stderr, "load failed: %s\n", load.error);
            return 1;
        }
        std::printf("driver ready, seed=0x%08X\n",
            static_cast<unsigned>(seed));

        Protocol protocol = DeriveProtocol(seed);
        Client client(protocol);
        if (!client.valid() || cpu >= sizeof(DWORD_PTR) * 8 ||
            SetThreadAffinityMask(
                GetCurrentThread(), static_cast<DWORD_PTR>(1) << cpu) == 0) {
            std::fprintf(stderr, "client/affinity failed\n");
            return 1;
        }
        SwitchToThread();
        if (!client.Register()) {
            std::fprintf(stderr, "register failed\n");
            return 1;
        }
        return CmdResolve(client, argv[2], argv[3]) ? 0 : 1;
    }

    if (argc >= 2 && std::strcmp(argv[1], "stop") == 0) {
        DriverConfig cfg{ L"JohnSmith", 0 };
        DriverUnload(cfg);
        std::puts("stopped.");
        return 0;
    }

    if (argc >= 2 && std::strcmp(argv[1], "start") == 0) {
        std::uint32_t seed = 0;
        std::uint32_t cpu = static_cast<std::uint32_t>(GetCurrentProcessorNumber());
        DriverConfig cfg{ L"JohnSmith", 0xFFFF };
        bool seedProvided = false;
        int dashDash = -1;

        for (int i = 2; i < argc; ++i) {
            if (std::strcmp(argv[i], "--") == 0) { dashDash = i; break; }
            if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
                std::uint64_t v;
                if (ParseU64(argv[++i], &v) && v <= UINT32_MAX && v != 0) {
                    seed = static_cast<std::uint32_t>(v);
                    seedProvided = true;
                }
            }
            else if (std::strcmp(argv[i], "--cpu") == 0 && i + 1 < argc) {
                std::uint64_t v;
                if (ParseU64(argv[++i], &v)) cpu = static_cast<std::uint32_t>(v);
            }
            else if (std::strcmp(argv[i], "--provider") == 0 && i + 1 < argc) {
                std::uint64_t v;
                if (ParseU64(argv[++i], &v)) cfg.provider = static_cast<std::uint32_t>(v);
            }
            else {
                std::fprintf(stderr, "unknown argument: %s\n", argv[i]);
                return 2;
            }
        }

        if (!seedProvided) {
            seed = GenerateSeed();
        }

        LoadResult lr = DriverLoad(cfg, seed);
        bool alreadyRunning = std::strcmp(lr.error, "already running") == 0;
        if (!lr.loaded) {
            std::fprintf(stderr, "load failed: %s\n", lr.error);
            return 1;
        }
        if (alreadyRunning && !seedProvided) {
            std::fprintf(stderr,
                "driver already running; pass its original --seed or stop it first\n");
            return 1;
        }
        std::printf("driver ready, seed=0x%08X\n", static_cast<unsigned>(seed));

        Protocol protocol = DeriveProtocol(seed);
        Client client(protocol);
        if (!client.valid()) {
            std::fprintf(stderr, "client init failed\n");
            return 1;
        }

        if (dashDash > 0 && argc - dashDash > 2) {
            if (cpu >= sizeof(DWORD_PTR) * 8 ||
                SetThreadAffinityMask(GetCurrentThread(),
                    static_cast<DWORD_PTR>(1) << cpu) == 0) {
                std::fprintf(stderr, "affinity failed\n");
                return 1;
            }
            SwitchToThread();
            if (!client.Register()) {
                std::fprintf(stderr, "register failed\n");
                return 1;
            }

            if (std::strcmp(argv[dashDash+1], "hook") == 0 &&
                std::strcmp(argv[dashDash+2], "install") == 0 &&
                argc - dashDash >= 4) {
                std::uint64_t va, cookie = 0;
                if (!ParseU64(argv[dashDash+3], &va)) return 2;
                if (argc - dashDash >= 5 &&
                    !ParseU64(argv[dashDash+4], &cookie)) return 2;
                CmdHookInstall(client, va, cookie);
            }
            else if (std::strcmp(argv[dashDash+1], "hook") == 0 &&
                     std::strcmp(argv[dashDash+2], "remove") == 0 &&
                     argc - dashDash >= 4) {
                std::uint64_t id;
                if (!ParseU64(argv[dashDash+3], &id) || id > UINT32_MAX) return 2;
                CmdHookRemove(client, static_cast<std::uint32_t>(id));
            }
            else if (std::strcmp(argv[dashDash+1], "hook") == 0 &&
                     std::strcmp(argv[dashDash+2], "probe") == 0 &&
                     argc - dashDash == 4) {
                std::uint64_t value;
                if (!ParseU64(argv[dashDash+3], &value)) return 2;
                if (!CmdHookProbe(client, value)) return 1;
            }
            else if (std::strcmp(argv[dashDash+1], "resolve") == 0 &&
                     argc - dashDash >= 4) {
                if (!CmdResolve(
                        client, argv[dashDash+2], argv[dashDash+3])) {
                    return 1;
                }
            }
            else {
                std::fprintf(stderr, "unknown one-shot command\n");
                return 2;
            }
            return 0;
        }

        return RunRepl(client, protocol, cpu);
    }

    PrintHelp();
    return 2;
}
