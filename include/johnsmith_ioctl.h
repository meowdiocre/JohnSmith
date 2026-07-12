#pragma once

#ifdef _KERNEL_MODE
#include <ntddk.h>
#else
#include <windows.h>
#include <winioctl.h>
#endif

#pragma pack(push, 1)

#define JOHNSMITH_DEVICE_NAME_W     L"\\Device\\JohnSmith"
#define JOHNSMITH_LINK_NAME_W       L"\\DosDevices\\JohnSmith"
#define JOHNSMITH_WIN32_PATH_W      L"\\\\.\\JohnSmith"

#define JOHNSMITH_IOCTL_FUNCTION_STATUS         0x800u
#define JOHNSMITH_IOCTL_FUNCTION_HOOK_INSTALL   0x801u
#define JOHNSMITH_IOCTL_FUNCTION_HOOK_REMOVE    0x802u
#define JOHNSMITH_IOCTL_FUNCTION_HOOK_QUERY     0x803u

#define IOCTL_JOHNSMITH_STATUS                              \
    CTL_CODE(FILE_DEVICE_UNKNOWN,                           \
             JOHNSMITH_IOCTL_FUNCTION_STATUS,               \
             METHOD_BUFFERED, FILE_READ_ACCESS)

#define IOCTL_JOHNSMITH_HOOK_INSTALL                        \
    CTL_CODE(FILE_DEVICE_UNKNOWN,                           \
             JOHNSMITH_IOCTL_FUNCTION_HOOK_INSTALL,         \
             METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)

#define IOCTL_JOHNSMITH_HOOK_REMOVE                         \
    CTL_CODE(FILE_DEVICE_UNKNOWN,                           \
             JOHNSMITH_IOCTL_FUNCTION_HOOK_REMOVE,          \
             METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)

#define IOCTL_JOHNSMITH_HOOK_QUERY                          \
    CTL_CODE(FILE_DEVICE_UNKNOWN,                           \
             JOHNSMITH_IOCTL_FUNCTION_HOOK_QUERY,           \
             METHOD_BUFFERED, FILE_READ_ACCESS)

#define JOHNSMITH_ABI_VERSION 1u

typedef struct _JOHNSMITH_REQUEST_HEADER {
    unsigned int Size;
    unsigned int Version;
    unsigned int Reserved;
} JOHNSMITH_REQUEST_HEADER;

#define JOHNSMITH_BACKEND_NAME_LENGTH 32u

typedef enum _JOHNSMITH_LIFECYCLE_WIRE {
    JohnSmithLifecycleStopped  = 0,
    JohnSmithLifecycleStarting = 1,
    JohnSmithLifecycleRunning  = 2,
    JohnSmithLifecycleStopping = 3
} JOHNSMITH_LIFECYCLE_WIRE;

typedef struct _JOHNSMITH_STATUS_REQUEST {
    JOHNSMITH_REQUEST_HEADER Header;
} JOHNSMITH_STATUS_REQUEST;

typedef struct _JOHNSMITH_STATUS_RESPONSE {
    unsigned int AbiVersion;
    unsigned int Lifecycle;
    unsigned int CpuCount;
    unsigned int RunningCpuCount;
    unsigned int BackendPresent;
    unsigned int Reserved;
    char BackendName[JOHNSMITH_BACKEND_NAME_LENGTH];
} JOHNSMITH_STATUS_RESPONSE;

#define JOHNSMITH_HOOK_MAX_PATCH 128u

typedef struct _JOHNSMITH_HOOK_INSTALL_REQUEST {
    JOHNSMITH_REQUEST_HEADER Header;
    unsigned long long GuestPhysicalAddress;
    unsigned int PatchOffset;
    unsigned int PatchSize;
    unsigned int Cookie;
    unsigned int Reserved;
    unsigned char PatchBytes[JOHNSMITH_HOOK_MAX_PATCH];
} JOHNSMITH_HOOK_INSTALL_REQUEST;

typedef struct _JOHNSMITH_HOOK_INSTALL_RESPONSE {
    unsigned int HookId;
    unsigned int Reserved;
} JOHNSMITH_HOOK_INSTALL_RESPONSE;

typedef struct _JOHNSMITH_HOOK_REMOVE_REQUEST {
    JOHNSMITH_REQUEST_HEADER Header;
    unsigned int HookId;
    unsigned int Reserved;
} JOHNSMITH_HOOK_REMOVE_REQUEST;

typedef struct _JOHNSMITH_HOOK_QUERY_REQUEST {
    JOHNSMITH_REQUEST_HEADER Header;
    unsigned int HookId;
    unsigned int Reserved;
} JOHNSMITH_HOOK_QUERY_REQUEST;

typedef struct _JOHNSMITH_HOOK_QUERY_RESPONSE {
    unsigned int Valid;
    unsigned int Kind;
    unsigned int Cookie;
    unsigned int Reserved;
    unsigned long long GuestPhysicalAddress;
    unsigned long long ShadowHostPhysicalAddress;
} JOHNSMITH_HOOK_QUERY_RESPONSE;

#pragma pack(pop)
