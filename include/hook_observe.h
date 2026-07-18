#pragma once

#include <ntddk.h>
#include "hv.h"

typedef VOID
(*HOOK_OBSERVE_CALLBACK)(
    _In_ ULONG HookId,
    _In_ ULONG64 CallerReturnAddress
    );

typedef struct _HOOK_OBSERVE_ENTRY {
    LIST_ENTRY Link;
    ULONG HookId;
    ULONG BackendHookId;
    volatile LONG Active;
    ULONG64 GuestPhysicalAddress;
    ULONG64 Cookie;
    PVOID ThunkVirtual;
    PVOID TrampolineVirtual;
    HOOK_OBSERVE_CALLBACK Callback;
    volatile LONG64 HitCount;
} HOOK_OBSERVE_ENTRY, *PHOOK_OBSERVE_ENTRY;

typedef struct _HOOK_OBSERVE_QUERY_ENTRY {
    ULONG HookId;
    ULONG Active;
    ULONG64 GuestPhysicalAddress;
    ULONG64 Cookie;
    LONG64 HitCount;
} HOOK_OBSERVE_QUERY_ENTRY, *PHOOK_OBSERVE_QUERY_ENTRY;

C_ASSERT(sizeof(HOOK_OBSERVE_QUERY_ENTRY) == 32);

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
ObserveHookInstall(
    _Inout_ HV_STATE* State,
    _In_ PVOID TargetVirtualAddress,
    _In_ ULONG64 Cookie,
    _In_ HOOK_OBSERVE_CALLBACK Callback,
    _Out_ ULONG* HookId
    );

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
ObserveHookRemove(
    _Inout_ HV_STATE* State,
    _In_ ULONG HookId
    );

NTSTATUS
ObserveHookQuery(
    _In_ ULONG HookId,
    _Out_ PHOOK_OBSERVE_QUERY_ENTRY Query
    );

ULONG
ObserveHookList(
    _Out_writes_to_(Capacity, return) PHOOK_OBSERVE_QUERY_ENTRY Queries,
    _In_ ULONG Capacity
    );

VOID
ObserveHookReset(
    VOID
    );

PVOID
HookObserveDispatch(
    _In_ ULONG HookId,
    _In_ ULONG64 CallerReturnAddress
    );


VOID
HookThunkInitialize(
    VOID
    );

VOID
HookThunkReset(
    VOID
    );

NTSTATUS
HookThunkAllocate(
    _Out_ PVOID* SlotVirtual,
    _Out_ ULONG* HookId
    );

VOID
HookThunkFree(
    _In_ PVOID SlotVirtual
    );

ULONG
HookThunkSlotSize(
    VOID
    );



_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
HookTrampolineBuild(
    _In_ PUCHAR OriginalVa,
    _In_ ULONG MinBytes,
    _Out_ PVOID* TrampolineVirtual,
    _Out_ ULONG* BytesCopied
    );

_IRQL_requires_(PASSIVE_LEVEL)
VOID
HookTrampolineFree(
    _In_ PVOID TrampolineVirtual
    );


VOID
AsmHookDispatcher(
    VOID
    );
