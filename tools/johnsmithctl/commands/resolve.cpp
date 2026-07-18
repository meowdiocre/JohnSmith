#include <windows.h>
#include <psapi.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "resolve.h"

#pragma comment(lib, "psapi.lib")

namespace {

constexpr std::uint32_t kMaxImageSize = 1024u * 1024u * 1024u;
constexpr std::uint32_t kMaxExportSize = 64u * 1024u * 1024u;
constexpr std::uint32_t kMaxExportCount = 1024u * 1024u;
constexpr std::size_t kMaxExportName = 4096;

enum class ResolveStatus {
    Success,
    NotFound,
    ReadFailed,
    InvalidImage
};

struct ResolvedExport {
    std::uint64_t address;
    std::uint32_t rva;
    bool forwarded;
    std::string forwarder;
};

bool AddAddress(std::uint64_t base, std::uint64_t offset, std::uint64_t* address)
{
    if (offset > UINT64_MAX - base) return false;
    *address = base + offset;
    return true;
}

bool RvaRangeValid(
    std::uint32_t rva,
    std::size_t size,
    std::uint32_t imageSize)
{
    return rva < imageSize && size <= imageSize - rva;
}

template<typename Reader, typename T>
bool ReadObject(Reader& read, std::uint64_t address, T* value)
{
    return read(address, value, sizeof(*value));
}

bool ExportRangeValid(
    std::uint32_t rva,
    std::size_t size,
    std::uint32_t exportRva,
    std::uint32_t exportSize)
{
    std::uint64_t start = rva;
    std::uint64_t base = exportRva;
    std::uint64_t end = base + exportSize;

    return start >= base && start <= end && size <= end - start;
}

ResolveStatus ReadExportString(
    const std::vector<std::uint8_t>& exportData,
    std::uint32_t exportRva,
    std::uint32_t rva,
    std::size_t limit,
    std::string* value)
{
    std::size_t offset;
    std::size_t available;
    const void* terminator;

    value->clear();
    if (!ExportRangeValid(rva, 1, exportRva,
            static_cast<std::uint32_t>(exportData.size()))) {
        return ResolveStatus::InvalidImage;
    }
    offset = rva - exportRva;
    available = std::min(limit, exportData.size() - offset);
    terminator = std::memchr(exportData.data() + offset, 0, available);
    if (terminator == nullptr) return ResolveStatus::InvalidImage;
    value->assign(
        reinterpret_cast<const char*>(exportData.data() + offset),
        reinterpret_cast<const char*>(terminator));
    return ResolveStatus::Success;
}

template<typename Reader>
ResolveStatus ResolveExportAtBase(
    Reader& read,
    std::uint64_t imageBase,
    const char* exportName,
    ResolvedExport* resolved)
{
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS64 nt{};
    IMAGE_EXPORT_DIRECTORY exports{};
    std::vector<std::uint32_t> functions;
    std::vector<std::uint32_t> names;
    std::vector<std::uint16_t> ordinals;
    std::vector<std::uint8_t> exportData;
    std::uint64_t address;
    std::uint32_t imageSize;
    std::uint32_t exportRva;
    std::uint32_t exportSize;

    *resolved = {};
    if (exportName == nullptr || *exportName == '\0' ||
        !ReadObject(read, imageBase, &dos)) {
        return ResolveStatus::ReadFailed;
    }
    if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0 ||
        dos.e_lfanew > 1024 * 1024 ||
        !AddAddress(imageBase, (std::uint32_t)dos.e_lfanew, &address) ||
        !ReadObject(read, address, &nt)) {
        return ResolveStatus::InvalidImage;
    }
    if (nt.Signature != IMAGE_NT_SIGNATURE ||
        nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt.OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXPORT) {
        return ResolveStatus::InvalidImage;
    }

    imageSize = nt.OptionalHeader.SizeOfImage;
    exportRva = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT]
        .VirtualAddress;
    exportSize = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT]
        .Size;
    /* ponytail: live kernel PE images are capped at 1 GiB; raise this ceiling
       if Windows ships a larger image. */
    if (imageSize == 0 || imageSize > kMaxImageSize || exportSize == 0 ||
        exportSize > kMaxExportSize ||
        !RvaRangeValid(exportRva, exportSize, imageSize) ||
        !RvaRangeValid(exportRva, sizeof(exports), imageSize) ||
        !AddAddress(imageBase, exportRva, &address)) {
        return ResolveStatus::InvalidImage;
    }
    exportData.resize(exportSize);
    if (!read(address, exportData.data(), exportData.size())) {
        return ResolveStatus::ReadFailed;
    }
    std::memcpy(&exports, exportData.data(), sizeof(exports));
    if (exports.NumberOfFunctions == 0 ||
        exports.NumberOfFunctions > kMaxExportCount ||
        exports.NumberOfNames > kMaxExportCount ||
        !ExportRangeValid(
            exports.AddressOfFunctions,
            (std::size_t)exports.NumberOfFunctions * sizeof(std::uint32_t),
            exportRva, exportSize) ||
        (exports.NumberOfNames != 0 &&
         (!ExportRangeValid(
              exports.AddressOfNames,
              (std::size_t)exports.NumberOfNames * sizeof(std::uint32_t),
              exportRva, exportSize) ||
          !ExportRangeValid(
              exports.AddressOfNameOrdinals,
              (std::size_t)exports.NumberOfNames * sizeof(std::uint16_t),
              exportRva, exportSize)))) {
        return ResolveStatus::InvalidImage;
    }

    functions.resize(exports.NumberOfFunctions);
    names.resize(exports.NumberOfNames);
    ordinals.resize(exports.NumberOfNames);
    std::memcpy(
        functions.data(),
        exportData.data() + exports.AddressOfFunctions - exportRva,
        functions.size() * sizeof(functions[0]));
    if (!names.empty()) {
        std::memcpy(
            names.data(),
            exportData.data() + exports.AddressOfNames - exportRva,
            names.size() * sizeof(names[0]));
        std::memcpy(
            ordinals.data(),
            exportData.data() + exports.AddressOfNameOrdinals - exportRva,
            ordinals.size() * sizeof(ordinals[0]));
    }

    for (std::size_t index = 0; index < names.size(); ++index) {
        std::string name;
        ResolveStatus status = ReadExportString(
            exportData, exportRva, names[index], kMaxExportName, &name);
        if (status != ResolveStatus::Success) return status;
        if (name != exportName) continue;
        if (ordinals[index] >= functions.size()) {
            return ResolveStatus::InvalidImage;
        }

        resolved->rva = functions[ordinals[index]];
        if (resolved->rva == 0 || resolved->rva >= imageSize) {
            return ResolveStatus::InvalidImage;
        }
        resolved->forwarded =
            resolved->rva >= exportRva && resolved->rva < exportRva + exportSize;
        if (resolved->forwarded) {
            std::size_t forwarderLimit = exportRva + exportSize - resolved->rva;
            return ReadExportString(
                exportData, exportRva, resolved->rva,
                forwarderLimit, &resolved->forwarder);
        }
        if (!AddAddress(imageBase, resolved->rva, &resolved->address)) {
            return ResolveStatus::InvalidImage;
        }
        return ResolveStatus::Success;
    }
    return ResolveStatus::NotFound;
}

void EnableDebugPrivilege()
{
    HANDLE token = nullptr;
    TOKEN_PRIVILEGES privileges{};

    if (!OpenProcessToken(
            GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY,
            &token)) {
        return;
    }
    if (LookupPrivilegeValueW(
            nullptr, SE_DEBUG_NAME, &privileges.Privileges[0].Luid)) {
        privileges.PrivilegeCount = 1;
        privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(
            token, FALSE, &privileges, sizeof(privileges), nullptr, nullptr);
    }
    CloseHandle(token);
}

std::uint64_t FindKernelModuleBase(const char* moduleName)
{
    std::vector<LPVOID> modules(256);
    DWORD needed = 0;

    EnableDebugPrivilege();
    if (!EnumDeviceDrivers(
            modules.data(),
            static_cast<DWORD>(modules.size() * sizeof(modules[0])),
            &needed)) {
        return 0;
    }
    if (needed > modules.size() * sizeof(modules[0])) {
        modules.resize((needed + sizeof(modules[0]) - 1) / sizeof(modules[0]));
        if (!EnumDeviceDrivers(
                modules.data(),
                static_cast<DWORD>(modules.size() * sizeof(modules[0])),
                &needed)) {
            return 0;
        }
    }

    std::size_t count = needed / sizeof(modules[0]);
    for (std::size_t index = 0; index < count; ++index) {
        char name[MAX_PATH];
        if (modules[index] != nullptr &&
            GetDeviceDriverBaseNameA(
                modules[index], name, static_cast<DWORD>(sizeof(name))) != 0 &&
            _stricmp(name, moduleName) == 0) {
            return reinterpret_cast<std::uint64_t>(modules[index]);
        }
    }
    return 0;
}

bool ReadKernelMemory(
    Client& client,
    std::uint64_t address,
    void* destination,
    std::size_t size)
{
    auto* output = static_cast<std::uint8_t*>(destination);
    std::size_t offset = 0;

    while (offset < size) {
        std::size_t chunk = std::min(
            size - offset, sizeof(client.page()->payload));
        std::uint64_t current;
        std::uint64_t args[4];
        std::uint64_t result;

        if (!AddAddress(address, offset, &current)) return false;
        args[0] = current;
        args[1] = chunk;
        args[2] = 0;
        args[3] = 0;
        if (!client.Issue(CommandRead, args, &result) ||
            static_cast<std::int32_t>(result) < 0) {
            return false;
        }
        std::memcpy(output + offset, client.page()->payload, chunk);
        offset += chunk;
    }
    return true;
}

} // namespace

bool CmdResolve(Client& client, const char* moduleName, const char* exportName)
{
    std::uint64_t imageBase;
    ResolvedExport resolved{};
    auto read = [&client](std::uint64_t address, void* buffer, std::size_t size) {
        return ReadKernelMemory(client, address, buffer, size);
    };

    if (moduleName == nullptr || *moduleName == '\0' ||
        exportName == nullptr || *exportName == '\0') {
        std::fprintf(stderr, "invalid module/export\n");
        return false;
    }
    imageBase = FindKernelModuleBase(moduleName);
    if (imageBase == 0) {
        std::fprintf(stderr, "kernel module not found: %s\n", moduleName);
        return false;
    }

    ResolveStatus status = ResolveExportAtBase(
        read, imageBase, exportName, &resolved);
    if (status == ResolveStatus::NotFound) {
        std::fprintf(stderr, "export not found: %s!%s\n", moduleName, exportName);
        return false;
    }
    if (status == ResolveStatus::ReadFailed) {
        std::fprintf(stderr, "kernel read failed while resolving %s!%s\n",
            moduleName, exportName);
        return false;
    }
    if (status != ResolveStatus::Success) {
        std::fprintf(stderr, "invalid live PE image: %s\n", moduleName);
        return false;
    }

    if (resolved.forwarded) {
        std::printf("%s!%s -> %s\n",
            moduleName, exportName, resolved.forwarder.c_str());
    } else {
        std::printf("%s!%s = 0x%016llX (base=0x%016llX, rva=0x%08X)\n",
            moduleName,
            exportName,
            static_cast<unsigned long long>(resolved.address),
            static_cast<unsigned long long>(imageBase),
            resolved.rva);
    }
    return true;
}

bool ResolveSelfTest()
{
    constexpr std::uint64_t base = 0x10000000ull;
    std::vector<std::uint8_t> image(0x1000);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(image.data());
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(image.data() + 0x80);
    auto* exports = reinterpret_cast<IMAGE_EXPORT_DIRECTORY*>(
        image.data() + 0x200);
    ResolvedExport resolved{};
    std::size_t reads = 0;

    dos->e_magic = IMAGE_DOS_SIGNATURE;
    dos->e_lfanew = 0x80;
    nt->Signature = IMAGE_NT_SIGNATURE;
    nt->FileHeader.Machine = IMAGE_FILE_MACHINE_AMD64;
    nt->OptionalHeader.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    nt->OptionalHeader.SizeOfImage = static_cast<DWORD>(image.size());
    nt->OptionalHeader.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
    nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT] = {
        0x200, 0x200
    };
    exports->NumberOfFunctions = 1;
    exports->NumberOfNames = 1;
    exports->AddressOfFunctions = 0x300;
    exports->AddressOfNames = 0x304;
    exports->AddressOfNameOrdinals = 0x308;
    *reinterpret_cast<std::uint32_t*>(image.data() + 0x300) = 0x500;
    *reinterpret_cast<std::uint32_t*>(image.data() + 0x304) = 0x320;
    *reinterpret_cast<std::uint16_t*>(image.data() + 0x308) = 0;
    std::memcpy(image.data() + 0x320, "NtYieldExecution", 17);

    auto read = [&image, &reads](
        std::uint64_t address, void* buffer, std::size_t size) {
        ++reads;
        if (address < base || address - base > image.size() ||
            size > image.size() - static_cast<std::size_t>(address - base)) {
            return false;
        }
        std::memcpy(
            buffer, image.data() + static_cast<std::size_t>(address - base), size);
        return true;
    };
    return ResolveExportAtBase(
               read, base, "NtYieldExecution", &resolved) ==
               ResolveStatus::Success &&
           !resolved.forwarded && resolved.rva == 0x500 &&
           resolved.address == base + 0x500 && reads == 3;
}
