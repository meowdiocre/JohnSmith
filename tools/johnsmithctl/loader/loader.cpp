#include <windows.h>
#include <cstdio>
#include <cstring>
#include "loader.h"
#include "dse.h"

static bool WriteSeed(const wchar_t* service, std::uint32_t seed)
{
    wchar_t keyPath[512];
    _snwprintf_s(keyPath, 512, _TRUNCATE,
        L"SYSTEM\\CurrentControlSet\\Services\\%s\\Parameters", service);

    HKEY key;
    LONG st = RegCreateKeyExW(HKEY_LOCAL_MACHINE, keyPath, 0, nullptr,
        REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &key, nullptr);
    if (st != ERROR_SUCCESS) return false;
    st = RegSetValueExW(key, L"HypercallSeed", 0, REG_DWORD,
        reinterpret_cast<const BYTE*>(&seed), sizeof(seed));
    RegCloseKey(key);
    return st == ERROR_SUCCESS;
}

static bool ResolveDriverPath(const wchar_t* service,
    wchar_t* out, std::size_t outLen)
{
    GetModuleFileNameW(nullptr, out, static_cast<DWORD>(outLen));
    wchar_t* sep = wcsrchr(out, L'\\');
    if (!sep) return false;
    wcscpy_s(sep + 1, outLen - (sep + 1 - out), service);
    wcscat_s(out, outLen, L".sys");
    return GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES;
}

LoadResult DriverLoad(const DriverConfig& cfg, std::uint32_t seed)
{
    LoadResult r{};
    r.seed = seed;

    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!scm) {
        std::snprintf(r.error, sizeof(r.error), "OpenSCManager failed: %lu", GetLastError());
        return r;
    }

    SC_HANDLE svc = OpenServiceW(scm, cfg.service, SERVICE_ALL_ACCESS);
    if (svc) {
        SERVICE_STATUS status;
        if (QueryServiceStatus(svc, &status) && status.dwCurrentState == SERVICE_RUNNING) {
            CloseServiceHandle(svc);
            CloseServiceHandle(scm);
            r.loaded = true;
            std::snprintf(r.error, sizeof(r.error), "already running");
            return r;
        }
    }

    wchar_t driverPath[MAX_PATH];
    if (!ResolveDriverPath(cfg.service, driverPath, MAX_PATH)) {
        std::snprintf(r.error, sizeof(r.error), "cannot resolve driver path");
        if (svc) CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        return r;
    }

    DseCtx dse{};
    if (!DseDisable(&dse, cfg.provider)) {
        std::snprintf(r.error, sizeof(r.error), "DSE: %s", dse.error);
        if (svc) CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        return r;
    }
    r.dseOriginal = dse.original;

    if (!svc) {
        svc = CreateServiceW(scm, cfg.service, L"JohnSmith Hypervisor",
            SERVICE_ALL_ACCESS, SERVICE_KERNEL_DRIVER,
            SERVICE_DEMAND_START, SERVICE_ERROR_NORMAL,
            driverPath, nullptr, nullptr, nullptr, nullptr, nullptr);
        if (!svc) {
            std::snprintf(r.error, sizeof(r.error), "CreateService failed: %lu", GetLastError());
            DseRestore(&dse);
            CloseServiceHandle(scm);
            return r;
        }
    } else {
        ChangeServiceConfigW(svc, SERVICE_KERNEL_DRIVER,
            SERVICE_DEMAND_START, SERVICE_ERROR_NORMAL,
            driverPath, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
    }

    if (!WriteSeed(cfg.service, seed)) {
        std::snprintf(r.error, sizeof(r.error), "seed write failed");
        DseRestore(&dse);
        DeleteService(svc);
        CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        return r;
    }

    if (!StartServiceW(svc, 0, nullptr)) {
        DWORD err = GetLastError();
        if (err != ERROR_SERVICE_ALREADY_RUNNING) {
            std::snprintf(r.error, sizeof(r.error), "StartService failed: %lu", err);
            DseRestore(&dse);
            DeleteService(svc);
            CloseServiceHandle(svc);
            CloseServiceHandle(scm);
            return r;
        }
    }

    CloseServiceHandle(svc);
    CloseServiceHandle(scm);

    if (!DseRestore(&dse)) {
        std::snprintf(r.error, sizeof(r.error), "WARNING: DSE restore failed");
    }

    r.loaded = true;
    return r;
}

bool DriverUnload(const DriverConfig& cfg)
{
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!scm) return false;

    SC_HANDLE svc = OpenServiceW(scm, cfg.service, SERVICE_ALL_ACCESS);
    if (svc) {
        SERVICE_STATUS status;
        ControlService(svc, SERVICE_CONTROL_STOP, &status);
        for (int i = 0; i < 100; ++i) {
            if (QueryServiceStatus(svc, &status) &&
                status.dwCurrentState == SERVICE_STOPPED) break;
            Sleep(100);
        }
        DeleteService(svc);
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);

    wchar_t keyPath[512];
    _snwprintf_s(keyPath, 512, _TRUNCATE,
        L"SYSTEM\\CurrentControlSet\\Services\\%s", cfg.service);
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, keyPath);

    return true;
}

std::uint32_t DriverStatus(const DriverConfig& cfg)
{
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE);
    if (!scm) return 0;
    SC_HANDLE svc = OpenServiceW(scm, cfg.service, SERVICE_QUERY_STATUS);
    if (!svc) { CloseServiceHandle(scm); return 0; }
    SERVICE_STATUS status;
    BOOL ok = QueryServiceStatus(svc, &status);
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return ok ? status.dwCurrentState : 0;
}
