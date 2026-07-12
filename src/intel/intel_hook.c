#include "intel_internal.h"

static INTEL_HOOK_POLICY g_HookTable[INTEL_HOOK_TABLE_CAPACITY];

BOOLEAN
IntelHookLookup(
    _In_ ULONG64 GuestPhysicalAddress,
    _Out_ INTEL_HOOK_POLICY* Out
    )
{
    ULONG64 target;
    ULONG index;

    RtlZeroMemory(Out, sizeof(*Out));

    target = GuestPhysicalAddress & ~((ULONG64)PAGE_SIZE - 1);

    for (index = 0; index < INTEL_HOOK_TABLE_CAPACITY; ++index) {
        ULONG64 slotGpa = (ULONG64)InterlockedCompareExchange64(
            (volatile LONG64*)&g_HookTable[index].GuestPhysicalAddress,
            0, 0);

        if (slotGpa == 0 || slotGpa != target) {
            continue;
        }

        Out->GuestPhysicalAddress = slotGpa;
        Out->ShadowHostPhysicalAddress =
            g_HookTable[index].ShadowHostPhysicalAddress;
        Out->ShadowVirtual = g_HookTable[index].ShadowVirtual;
        Out->OriginalPrimaryPte = g_HookTable[index].OriginalPrimaryPte;
        Out->OriginalSecondaryPte = g_HookTable[index].OriginalSecondaryPte;
        Out->Kind = g_HookTable[index].Kind;
        Out->Cookie = g_HookTable[index].Cookie;
        return Out->Kind != INTEL_HOOK_KIND_NONE;
    }
    return FALSE;
}

VOID
IntelHookResetTable(
    VOID
    )
{
    RtlZeroMemory(g_HookTable, sizeof(g_HookTable));
}

NTSTATUS
IntelHookQuery(
    _In_ HV_STATE* State,
    _In_ ULONG HookId,
    _Out_ ULONG* Valid,
    _Out_ ULONG* Kind,
    _Out_ ULONG* Cookie,
    _Out_ ULONG64* GuestPhysicalAddress,
    _Out_ ULONG64* ShadowHostPhysicalAddress
    )
{
    ULONG64 slotGpa;

    UNREFERENCED_PARAMETER(State);
    *Valid = 0;
    *Kind = 0;
    *Cookie = 0;
    *GuestPhysicalAddress = 0;
    *ShadowHostPhysicalAddress = 0;

    if (HookId >= INTEL_HOOK_TABLE_CAPACITY) {
        return STATUS_INVALID_PARAMETER;
    }

    slotGpa = (ULONG64)InterlockedCompareExchange64(
        (volatile LONG64*)&g_HookTable[HookId].GuestPhysicalAddress, 0, 0);
    if (slotGpa == 0) {
        return STATUS_SUCCESS;
    }
    *Valid = 1;
    *Kind = g_HookTable[HookId].Kind;
    *Cookie = g_HookTable[HookId].Cookie;
    *GuestPhysicalAddress = slotGpa;
    *ShadowHostPhysicalAddress =
        g_HookTable[HookId].ShadowHostPhysicalAddress;
    return STATUS_SUCCESS;
}

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
IntelHookEnsureSecondaryRoot(
    _Inout_ INTEL_BACKEND_CONTEXT* Backend
    )
{
    INTEL_EPT_ROOT* root;
    NTSTATUS status;

    if (Backend->HookRoot != NULL) {
        return STATUS_SUCCESS;
    }

    root = (INTEL_EPT_ROOT*)ExAllocatePool2(
        POOL_FLAG_NON_PAGED, sizeof(*root), HV_POOL_TAG_HOOK);
    if (root == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    status = IntelBuildIdentityRoot(
        Backend, root, HV_PAGE_ACCESS_READ | HV_PAGE_ACCESS_WRITE);
    if (!NT_SUCCESS(status)) {
        IntelFreeRoot(root);
        ExFreePoolWithTag(root, HV_POOL_TAG_HOOK);
        return status;
    }
    Backend->HookRoot = root;
    return STATUS_SUCCESS;
}

static ULONG
IntelHookFindFreeSlot(
    VOID
    )
{
    ULONG index;

    for (index = 0; index < INTEL_HOOK_TABLE_CAPACITY; ++index) {
        if (InterlockedCompareExchange64(
                (volatile LONG64*)&g_HookTable[index].GuestPhysicalAddress,
                0, 0) == 0) {
            return index;
        }
    }
    return INTEL_HOOK_TABLE_CAPACITY;
}

static NTSTATUS
IntelHookBuildShadow(
    _In_ ULONG64 GuestPhysicalAddress,
    _In_reads_bytes_(PatchSize) const VOID* PatchBytes,
    _In_ ULONG PatchOffset,
    _In_ ULONG PatchSize,
    _Outptr_ PVOID* ShadowVirtual,
    _Out_ ULONG64* ShadowPhysical
    )
{
    PHYSICAL_ADDRESS originalPhysical;
    PVOID originalMapping;
    PVOID shadow;

    *ShadowVirtual = NULL;
    *ShadowPhysical = 0;

    shadow = IntelAllocatePage(MAXLONGLONG);
    if (shadow == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    originalPhysical.QuadPart = (LONGLONG)(GuestPhysicalAddress &
                                           ~((ULONG64)PAGE_SIZE - 1));
    originalMapping = MmMapIoSpaceEx(
        originalPhysical, PAGE_SIZE, PAGE_READWRITE);
    if (originalMapping == NULL) {
        MmFreeContiguousMemory(shadow);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlCopyMemory(shadow, originalMapping, PAGE_SIZE);
    MmUnmapIoSpace(originalMapping, PAGE_SIZE);

    RtlCopyMemory((PUCHAR)shadow + PatchOffset, PatchBytes, PatchSize);

    *ShadowVirtual = shadow;
    *ShadowPhysical = (ULONG64)MmGetPhysicalAddress(shadow).QuadPart;
    return STATUS_SUCCESS;
}

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
IntelHookInstall(
    _Inout_ HV_STATE* State,
    _In_ ULONG64 GuestPhysicalAddress,
    _In_reads_bytes_(PatchSize) const VOID* PatchBytes,
    _In_ ULONG PatchOffset,
    _In_ ULONG PatchSize,
    _In_ ULONG Cookie,
    _Out_ ULONG* HookId
    )
{
    INTEL_BACKEND_CONTEXT* backend;
    PVOID shadowVirtual = NULL;
    ULONG64 shadowPhysical = 0;
    ULONG64* primaryPt = NULL;
    ULONG64* secondaryPt = NULL;
    ULONG primaryPtIndex = 0;
    ULONG secondaryPtIndex = 0;
    ULONG slot;
    ULONG64 alignedGpa;
    ULONG64 originalPrimary = 0;
    ULONG64 originalSecondary = 0;
    NTSTATUS status;
    BOOLEAN backendLockHeld = FALSE;
    BOOLEAN primaryLocked = FALSE;
    BOOLEAN secondaryLocked = FALSE;

    if (State == NULL || HookId == NULL || PatchBytes == NULL ||
        PatchSize == 0 || State->BackendContext == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if ((ULONG64)PatchOffset + (ULONG64)PatchSize > PAGE_SIZE) {
        return STATUS_INVALID_PARAMETER;
    }
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return STATUS_INVALID_DEVICE_STATE;
    }

    *HookId = INTEL_HOOK_TABLE_CAPACITY;
    backend = (INTEL_BACKEND_CONTEXT*)State->BackendContext;

    if ((backend->EptVpidCapabilities & 1) == 0) {
        return STATUS_NOT_SUPPORTED;
    }

    alignedGpa = GuestPhysicalAddress & ~((ULONG64)PAGE_SIZE - 1);
    if (alignedGpa == 0) {

        return STATUS_INVALID_ADDRESS;
    }

    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&backend->HookLock);
    backendLockHeld = TRUE;

    status = IntelHookEnsureSecondaryRoot(backend);
    if (!NT_SUCCESS(status)) {
        goto Exit;
    }

    slot = IntelHookFindFreeSlot();
    if (slot == INTEL_HOOK_TABLE_CAPACITY) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto Exit;
    }

    status = IntelHookBuildShadow(
        alignedGpa, PatchBytes, PatchOffset, PatchSize,
        &shadowVirtual, &shadowPhysical);
    if (!NT_SUCCESS(status)) {
        goto Exit;
    }

    status = IntelHookAcquirePt(
        &backend->PrimaryRoot, backend->MapLimit, alignedGpa,
        INTEL_SPLIT_REASON_HOOK, &primaryPt, &primaryPtIndex);
    if (!NT_SUCCESS(status)) {
        goto Exit;
    }
    primaryLocked = TRUE;

    status = IntelHookAcquirePt(
        backend->HookRoot, backend->MapLimit, alignedGpa,
        INTEL_SPLIT_REASON_HOOK, &secondaryPt, &secondaryPtIndex);
    if (!NT_SUCCESS(status)) {
        goto Exit;
    }
    secondaryLocked = TRUE;

    originalPrimary = primaryPt[primaryPtIndex];
    originalSecondary = secondaryPt[secondaryPtIndex];

    InterlockedExchange64(
        (volatile LONG64*)&primaryPt[primaryPtIndex],
        (LONG64)((originalPrimary & ~EPT_ACCESS_MASK) |
                 HV_PAGE_ACCESS_READ | HV_PAGE_ACCESS_WRITE));
    InterlockedExchange64(
        (volatile LONG64*)&secondaryPt[secondaryPtIndex],
        (LONG64)(((originalSecondary & EPT_MEMORY_TYPE_MASK)) |
                 (shadowPhysical & EPT_ADDRESS_MASK) |
                 HV_PAGE_ACCESS_EXECUTE));

    KeMemoryBarrier();

    g_HookTable[slot].ShadowHostPhysicalAddress = shadowPhysical;
    g_HookTable[slot].ShadowVirtual = shadowVirtual;
    g_HookTable[slot].OriginalPrimaryPte = originalPrimary;
    g_HookTable[slot].OriginalSecondaryPte = originalSecondary;
    g_HookTable[slot].Kind = INTEL_HOOK_KIND_EXECUTE;
    g_HookTable[slot].Cookie = Cookie;
    KeMemoryBarrier();
    InterlockedExchange64(
        (volatile LONG64*)&g_HookTable[slot].GuestPhysicalAddress,
        (LONG64)alignedGpa);

    IntelHookReleasePt(backend->HookRoot);
    secondaryLocked = FALSE;
    IntelHookReleasePt(&backend->PrimaryRoot);
    primaryLocked = FALSE;

    ExReleasePushLockExclusive(&backend->HookLock);
    KeLeaveCriticalRegion();
    backendLockHeld = FALSE;

    IntelHookInvalidateEverywhere(State, backend);

    *HookId = slot;
    return STATUS_SUCCESS;

Exit:
    if (secondaryLocked) {
        IntelHookReleasePt(backend->HookRoot);
    }
    if (primaryLocked) {
        IntelHookReleasePt(&backend->PrimaryRoot);
    }
    if (shadowVirtual != NULL) {
        MmFreeContiguousMemory(shadowVirtual);
    }
    if (backendLockHeld) {
        ExReleasePushLockExclusive(&backend->HookLock);
        KeLeaveCriticalRegion();
    }
    return status;
}

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
IntelHookRemove(
    _Inout_ HV_STATE* State,
    _In_ ULONG HookId
    )
{
    INTEL_BACKEND_CONTEXT* backend;
    ULONG64* primaryPt = NULL;
    ULONG64* secondaryPt = NULL;
    ULONG primaryPtIndex = 0;
    ULONG secondaryPtIndex = 0;
    ULONG64 alignedGpa;
    ULONG64 originalPrimary;
    ULONG64 originalSecondary;
    PVOID shadowVirtual;
    NTSTATUS status;

    if (State == NULL || State->BackendContext == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    if (HookId >= INTEL_HOOK_TABLE_CAPACITY) {
        return STATUS_INVALID_PARAMETER;
    }
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    backend = (INTEL_BACKEND_CONTEXT*)State->BackendContext;

    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&backend->HookLock);

    alignedGpa = (ULONG64)InterlockedCompareExchange64(
        (volatile LONG64*)&g_HookTable[HookId].GuestPhysicalAddress, 0, 0);
    if (alignedGpa == 0) {
        ExReleasePushLockExclusive(&backend->HookLock);
        KeLeaveCriticalRegion();
        return STATUS_NOT_FOUND;
    }

    originalPrimary = g_HookTable[HookId].OriginalPrimaryPte;
    originalSecondary = g_HookTable[HookId].OriginalSecondaryPte;
    shadowVirtual = g_HookTable[HookId].ShadowVirtual;

    InterlockedExchange64(
        (volatile LONG64*)&g_HookTable[HookId].GuestPhysicalAddress, 0);
    KeMemoryBarrier();

    ExReleasePushLockExclusive(&backend->HookLock);
    KeLeaveCriticalRegion();

    IntelHookInvalidateEverywhere(State, backend);

    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&backend->HookLock);

    status = IntelHookAcquirePt(
        &backend->PrimaryRoot, backend->MapLimit, alignedGpa,
        INTEL_SPLIT_REASON_HOOK, &primaryPt, &primaryPtIndex);
    if (NT_SUCCESS(status)) {
        InterlockedExchange64(
            (volatile LONG64*)&primaryPt[primaryPtIndex],
            (LONG64)originalPrimary);
        IntelHookReleasePt(&backend->PrimaryRoot);
    }

    if (backend->HookRoot != NULL) {
        status = IntelHookAcquirePt(
            backend->HookRoot, backend->MapLimit, alignedGpa,
            INTEL_SPLIT_REASON_HOOK, &secondaryPt, &secondaryPtIndex);
        if (NT_SUCCESS(status)) {
            InterlockedExchange64(
                (volatile LONG64*)&secondaryPt[secondaryPtIndex],
                (LONG64)originalSecondary);
            IntelHookReleasePt(backend->HookRoot);
        }
    }

    g_HookTable[HookId].ShadowHostPhysicalAddress = 0;
    g_HookTable[HookId].ShadowVirtual = NULL;
    g_HookTable[HookId].OriginalPrimaryPte = 0;
    g_HookTable[HookId].OriginalSecondaryPte = 0;
    g_HookTable[HookId].Kind = INTEL_HOOK_KIND_NONE;
    g_HookTable[HookId].Cookie = 0;

    ExReleasePushLockExclusive(&backend->HookLock);
    KeLeaveCriticalRegion();

    IntelHookInvalidateEverywhere(State, backend);

    if (shadowVirtual != NULL) {
        MmFreeContiguousMemory(shadowVirtual);
    }
    return STATUS_SUCCESS;
}
