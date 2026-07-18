#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include "dse.h"

struct SpawnResult {
    bool        success;
    DWORD       original;
    std::string output;
};

static bool FindKduPath(wchar_t* path, size_t len)
{
    GetModuleFileNameW(nullptr, path, static_cast<DWORD>(len));
    wchar_t* sep = wcsrchr(path, L'\\');
    if (!sep) return false;
    wcscpy_s(sep + 1, len - (sep + 1 - path), L"kdu.exe");
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

static SpawnResult RunKdu(DWORD value, DWORD provider)
{
    SpawnResult r{};
    wchar_t kduPath[MAX_PATH];

    if (!FindKduPath(kduPath, MAX_PATH)) {
        r.output = "kdu.exe not found alongside johnsmithctl.exe";
        return r;
    }

    wchar_t cmdLine[512];
    if (provider != 0xFFFF)
        _snwprintf_s(cmdLine, 512, _TRUNCATE,
            L"\"%s\" -dse %lu -prv %lu", kduPath, value, provider);
    else
        _snwprintf_s(cmdLine, 512, _TRUNCATE,
            L"\"%s\" -dse %lu", kduPath, value);

    HANDLE hRead, hWrite;
    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) {
        r.output = "CreatePipe failed";
        return r;
    }

    PROCESS_INFORMATION pi{};
    STARTUPINFOW si{ sizeof(si) };
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hWrite;
    si.hStdError  = hWrite;

    if (!CreateProcessW(nullptr, cmdLine, nullptr, nullptr,
            TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        r.output = "CreateProcess failed";
        CloseHandle(hRead); CloseHandle(hWrite);
        return r;
    }

    CloseHandle(hWrite);
    WaitForSingleObject(pi.hProcess, 30000);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    char buf[4096]{};
    DWORD bytesRead;
    if (ReadFile(hRead, buf, sizeof(buf) - 1, &bytesRead, nullptr))
        r.output = buf;
    CloseHandle(hRead);

    if (r.output.find("verification succeeded") == std::string::npos) {
        r.output = "KDU failed: " + r.output;
        return r;
    }

    auto pos = r.output.find("value: ");
    if (pos != std::string::npos)
        r.original = static_cast<DWORD>(
            strtoul(r.output.c_str() + pos + 7, nullptr, 16));

    r.success = true;
    return r;
}

bool DseDisable(DseCtx* ctx, std::uint32_t providerId)
{
    ctx->original = 0;
    ctx->provider = providerId;

    SpawnResult r = RunKdu(0, providerId);
    if (!r.success) {
        snprintf(ctx->error, sizeof(ctx->error), "%s", r.output.c_str());
        return false;
    }
    ctx->original = r.original;
    return true;
}

bool DseRestore(DseCtx* ctx)
{
    SpawnResult r = RunKdu(ctx->original, ctx->provider);
    return r.success;
}

void DseListProviders()
{
    std::puts(
        "KDU provider database (drv64.dll) — 65 providers available.\n"
        "  --provider 0  Intel NAL (iQVM64.sys, default)\n"
        "  --provider 1  RTCore64 (MSI Afterburner/RTSS)\n"
        "  --provider 2  GDRV (Gigabyte)\n"
        "  ...\n"
        "Use --provider <id> to select, or omit for auto (provider 0).");
}

std::uint32_t DseAutoSelect() { return 0; }
