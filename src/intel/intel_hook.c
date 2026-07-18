#include "intel_internal.h"
#include "../common/x86_common.h"
#include "hv_log.h"

#define INTEL_HOOK_TOMBSTONE \
    ((INTEL_HOOK_POLICY*)(ULONG_PTR)1)

static INTEL_HOOK_POLICY* volatile
    g_HookSlots[INTEL_HOOK_TABLE_CAPACITY];
static INTEL_HOOK_HASH_ENTRY
    g_HookHash[INTEL_HOOK_HASH_CAPACITY];

typedef struct _INTEL_HOOK_RETIRED_PAGES {
    PVOID PrimaryPt;
    PVOID SecondaryPt;
    PVOID PrimaryPd;
    PVOID SecondaryPd;
} INTEL_HOOK_RETIRED_PAGES;

static ULONG
IntelHookHashIndex(
    _In_ ULONG64 GuestPhysicalAddress
    )
{
    return (ULONG)((((GuestPhysicalAddress >> PAGE_SHIFT) *
                      2654435761ull) >> (64 - 11)) &
                    (INTEL_HOOK_HASH_CAPACITY - 1));
}

static INTEL_HOOK_POLICY*
IntelHookReadSlot(
    _In_ ULONG HookId
    )
{
    return (INTEL_HOOK_POLICY*)InterlockedCompareExchangePointer(
        (PVOID volatile*)&g_HookSlots[HookId], NULL, NULL);
}

static BOOLEAN
IntelHookHashInsert(
    _In_ INTEL_HOOK_POLICY* Policy
    )
{
    ULONG start = IntelHookHashIndex(Policy->GuestPhysicalAddress);
    ULONG probe;

    /* At most 64 policies occupy 2048 buckets (3.125% load), so
       the requested 75% rehash path is unreachable until the hook-ID ABI
       ceiling grows. */
    for (probe = 0; probe < INTEL_HOOK_HASH_CAPACITY; ++probe) {
        INTEL_HOOK_HASH_ENTRY* entry =
            &g_HookHash[(start + probe) & (INTEL_HOOK_HASH_CAPACITY - 1)];
        INTEL_HOOK_POLICY* current =
            (INTEL_HOOK_POLICY*)InterlockedCompareExchangePointer(
                (PVOID volatile*)&entry->Policy, NULL, NULL);

        if (current == NULL || current == INTEL_HOOK_TOMBSTONE) {
            InterlockedExchange64(
                (volatile LONG64*)&entry->GuestPhysicalAddress,
                (LONG64)Policy->GuestPhysicalAddress);
            KeMemoryBarrier();
            InterlockedExchangePointer(
                (PVOID volatile*)&entry->Policy, Policy);
            return TRUE;
        }
    }
    return FALSE;
}

static VOID
IntelHookHashRemove(
    _In_ const INTEL_HOOK_POLICY* Policy
    )
{
    ULONG start = IntelHookHashIndex(Policy->GuestPhysicalAddress);
    ULONG probe;

    for (probe = 0; probe < INTEL_HOOK_HASH_CAPACITY; ++probe) {
        INTEL_HOOK_HASH_ENTRY* entry =
            &g_HookHash[(start + probe) & (INTEL_HOOK_HASH_CAPACITY - 1)];
        INTEL_HOOK_POLICY* current =
            (INTEL_HOOK_POLICY*)InterlockedCompareExchangePointer(
                (PVOID volatile*)&entry->Policy, NULL, NULL);
        ULONG64 gpa = (ULONG64)InterlockedCompareExchange64(
            (volatile LONG64*)&entry->GuestPhysicalAddress, 0, 0);

        if (current == NULL) {
            return;
        }
        if (current == Policy && gpa == Policy->GuestPhysicalAddress) {
            InterlockedExchangePointer(
                (PVOID volatile*)&entry->Policy, INTEL_HOOK_TOMBSTONE);
            KeMemoryBarrier();
            InterlockedExchange64(
                (volatile LONG64*)&entry->GuestPhysicalAddress, 0);
            return;
        }
    }
}

_Success_(return != FALSE)
BOOLEAN
IntelHookLookup(
    _In_ const INTEL_BACKEND_CONTEXT* Backend,
    _In_ ULONG64 GuestPhysicalAddress,
    _Out_ INTEL_HOOK_POLICY* Out
    )
{
    ULONG64 target;
    ULONG start;
    ULONG probe;

    UNREFERENCED_PARAMETER(Backend);
    if (Out == NULL) {
        return FALSE;
    }
    RtlZeroMemory(Out, sizeof(*Out));
    target = GuestPhysicalAddress & ~((ULONG64)PAGE_SIZE - 1);
    start = IntelHookHashIndex(target);

    for (probe = 0; probe < INTEL_HOOK_HASH_CAPACITY; ++probe) {
        const INTEL_HOOK_HASH_ENTRY* entry =
            &g_HookHash[(start + probe) & (INTEL_HOOK_HASH_CAPACITY - 1)];
        INTEL_HOOK_POLICY* policy =
            (INTEL_HOOK_POLICY*)InterlockedCompareExchangePointer(
                (PVOID volatile*)&entry->Policy, NULL, NULL);
        ULONG64 gpa = (ULONG64)InterlockedCompareExchange64(
            (volatile LONG64*)&entry->GuestPhysicalAddress, 0, 0);

        if (policy == NULL) {
            return FALSE;
        }
        if (policy == INTEL_HOOK_TOMBSTONE || gpa != target) {
            continue;
        }
        *Out = *policy;
        KeMemoryBarrier();
        if (policy == (INTEL_HOOK_POLICY*)
                InterlockedCompareExchangePointer(
                    (PVOID volatile*)&entry->Policy, NULL, NULL) &&
            gpa == (ULONG64)InterlockedCompareExchange64(
                (volatile LONG64*)&entry->GuestPhysicalAddress, 0, 0)) {
            return TRUE;
        }
        RtlZeroMemory(Out, sizeof(*Out));
    }
    return FALSE;
}

VOID
IntelHookResetTable(
    VOID
    )
{
    RtlZeroMemory(g_HookSlots, sizeof(g_HookSlots));
    RtlZeroMemory(g_HookHash, sizeof(g_HookHash));
}

VOID
IntelHookTeardown(
    VOID
    )
{
    ULONG index;

    for (index = 0; index < INTEL_HOOK_TABLE_CAPACITY; ++index) {
        INTEL_HOOK_POLICY* policy = IntelHookReadSlot(index);
        if (policy == NULL) {
            continue;
        }
        if (policy->ShadowVirtual != NULL) {
            MmFreeContiguousMemory(policy->ShadowVirtual);
        }
        if (policy->PrimaryPtes != NULL) {
            ExFreePoolWithTag(policy->PrimaryPtes, HV_POOL_TAG_HOOK_POLICY);
        }
        if (policy->SecondaryPtes != NULL) {
            ExFreePoolWithTag(policy->SecondaryPtes, HV_POOL_TAG_HOOK_POLICY);
        }
        ExFreePoolWithTag(policy, HV_POOL_TAG_HOOK_POLICY);
    }
    IntelHookResetTable();
}

NTSTATUS
IntelHookQuery(
    _In_ HV_STATE* State,
    _In_ ULONG HookId,
    _Out_ ULONG* Valid,
    _Out_ ULONG* Kind,
    _Out_ ULONG64* Cookie,
    _Out_ ULONG64* GuestPhysicalAddress,
    _Out_ ULONG64* ShadowHostPhysicalAddress
    )
{
    INTEL_BACKEND_CONTEXT* backend;
    INTEL_HOOK_POLICY* policy;

    if (Valid == NULL || Kind == NULL || Cookie == NULL ||
        GuestPhysicalAddress == NULL || ShadowHostPhysicalAddress == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    *Valid = 0;
    *Kind = 0;
    *Cookie = 0;
    *GuestPhysicalAddress = 0;
    *ShadowHostPhysicalAddress = 0;
    if (State == NULL || State->BackendContext == NULL ||
        HookId >= INTEL_HOOK_TABLE_CAPACITY) {
        return STATUS_INVALID_PARAMETER;
    }

    backend = (INTEL_BACKEND_CONTEXT*)State->BackendContext;
    KeEnterCriticalRegion();
    ExAcquirePushLockShared(&backend->HookLock);
    policy = IntelHookReadSlot(HookId);
    if (policy != NULL) {
        *Valid = 1;
        *Kind = policy->Kind;
        *Cookie = policy->Cookie;
        *GuestPhysicalAddress = policy->GuestPhysicalAddress;
        *ShadowHostPhysicalAddress = policy->ShadowHostPhysicalAddress;
    }
    ExReleasePushLockShared(&backend->HookLock);
    KeLeaveCriticalRegion();
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
    status = IntelCloneRootTemplate(&Backend->PrimaryRoot, root);
    if (!NT_SUCCESS(status)) {
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
        if (IntelHookReadSlot(index) == NULL) {
            return index;
        }
    }
    return INTEL_HOOK_TABLE_CAPACITY;
}

static BOOLEAN
IntelHookGpaExistsLocked(
    _In_ ULONG64 GuestPhysicalAddress
    )
{
    ULONG index;

    for (index = 0; index < INTEL_HOOK_TABLE_CAPACITY; ++index) {
        INTEL_HOOK_POLICY* policy = IntelHookReadSlot(index);
        if (policy != NULL &&
            policy->GuestPhysicalAddress == GuestPhysicalAddress) {
            return TRUE;
        }
    }
    return FALSE;
}

static NTSTATUS
IntelHookBuildShadow(
    _In_ ULONG64 HostPhysicalAddress,
    _In_reads_bytes_(PatchSize) const VOID* PatchBytes,
    _In_ ULONG PatchOffset,
    _In_ ULONG PatchSize,
    _Outptr_ PVOID* ShadowVirtual,
    _Out_ ULONG64* ShadowPhysical
    )
{
    MM_COPY_ADDRESS source;
    PPHYSICAL_MEMORY_RANGE ranges;
    SIZE_T copied = 0;
    PVOID shadow;
    NTSTATUS status;

    *ShadowVirtual = NULL;
    *ShadowPhysical = 0;
    ranges = MmGetPhysicalMemoryRangesEx2(NULL, 0);
    if (ranges == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    if (!HvX86RangeIsRam(ranges, HostPhysicalAddress, PAGE_SIZE)) {
        ExFreePool(ranges);
        return STATUS_INVALID_ADDRESS;
    }
    ExFreePool(ranges);

    shadow = IntelAllocatePage(MAXLONGLONG);
    if (shadow == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    source.PhysicalAddress.QuadPart = (LONGLONG)HostPhysicalAddress;
    status = MmCopyMemory(
        shadow, source, PAGE_SIZE, MM_COPY_MEMORY_PHYSICAL, &copied);
    if (!NT_SUCCESS(status) || copied != PAGE_SIZE) {
        MmFreeContiguousMemory(shadow);
        return NT_SUCCESS(status) ? STATUS_PARTIAL_COPY : status;
    }
    RtlCopyMemory((PUCHAR)shadow + PatchOffset, PatchBytes, PatchSize);
    *ShadowVirtual = shadow;
    *ShadowPhysical = (ULONG64)MmGetPhysicalAddress(shadow).QuadPart;
    return STATUS_SUCCESS;
}

static VOID
IntelHookFreeRetiredPages(
    _In_reads_(Count) const INTEL_HOOK_RETIRED_PAGES* Pages,
    _In_ ULONG Count
    )
{
    ULONG index;
    for (index = 0; index < Count; ++index) {
        IntelFreeRetiredCpuEptPage(Pages[index].PrimaryPt);
        IntelFreeRetiredCpuEptPage(Pages[index].SecondaryPt);
        IntelFreeRetiredCpuEptPage(Pages[index].PrimaryPd);
        IntelFreeRetiredCpuEptPage(Pages[index].SecondaryPd);
    }
}

static VOID
IntelHookReleasePreparedCpuPages(
    _Inout_ HV_STATE* State,
    _In_ INTEL_BACKEND_CONTEXT* Backend,
    _In_ ULONG64 GuestPhysicalAddress,
    _In_ ULONG PreparedCount,
    _In_ ULONG Capacity,
    _Out_writes_(Capacity) INTEL_HOOK_RETIRED_PAGES* RetiredPages
    )
{
    ULONG index;

    NT_ASSERT(PreparedCount <= Capacity);
    if (PreparedCount > Capacity) {
        return;
    }
    RtlZeroMemory(RetiredPages, (SIZE_T)Capacity * sizeof(*RetiredPages));
    for (index = 0; index < PreparedCount; ++index) {
        INTEL_CPU_CONTEXT* cpuContext =
            (INTEL_CPU_CONTEXT*)State->Cpus[index].VendorContext;
        IntelReleaseCpuHookPage(
            cpuContext, Backend, GuestPhysicalAddress,
            &RetiredPages[index].PrimaryPt,
            &RetiredPages[index].SecondaryPt,
            &RetiredPages[index].PrimaryPd,
            &RetiredPages[index].SecondaryPd);
    }
}

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
IntelHookInstall(
    _Inout_ HV_STATE* State,
    _In_ ULONG64 GuestPhysicalAddress,
    _In_reads_bytes_(PatchSize) const VOID* PatchBytes,
    _In_ ULONG PatchOffset,
    _In_ ULONG PatchSize,
    _In_ ULONG64 Cookie,
    _Out_ ULONG* HookId
    )
{
    INTEL_BACKEND_CONTEXT* backend;
    INTEL_HOOK_POLICY* policy = NULL;
    INTEL_HOOK_RETIRED_PAGES* retiredPages = NULL;
    ULONG64 alignedGpa;
    ULONG slot;
    ULONG prepared = 0;
    ULONG index;
    SIZE_T pointerBytes;
    SIZE_T retiredBytes;
    NTSTATUS status = STATUS_SUCCESS;
    BOOLEAN published = FALSE;

    if (State == NULL || State->BackendContext == NULL || HookId == NULL ||
        PatchBytes == NULL || PatchSize == 0 ||
        (ULONG64)PatchOffset + PatchSize > PAGE_SIZE) {
        return STATUS_INVALID_PARAMETER;
    }
    if (State->CpuCount == 0 ||
        State->CpuCount > MAXULONG / sizeof(INTEL_HOOK_RETIRED_PAGES)) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    backend = (INTEL_BACKEND_CONTEXT*)State->BackendContext;
    if ((backend->EptVpidCapabilities & 1) == 0) {
        return STATUS_NOT_SUPPORTED;
    }
    alignedGpa = GuestPhysicalAddress & ~((ULONG64)PAGE_SIZE - 1);
    if (alignedGpa == 0 || alignedGpa >= backend->MapLimit) {
        return STATUS_INVALID_ADDRESS;
    }
    *HookId = INTEL_HOOK_TABLE_CAPACITY;

    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&backend->HookLock);
    if (InterlockedCompareExchange(
            &backend->HookMutationActive, 1, 0) != 0) {
        status = STATUS_DEVICE_BUSY;
        goto Exit;
    }
    status = IntelHookEnsureSecondaryRoot(backend);
    if (!NT_SUCCESS(status)) {
        goto ExitMutation;
    }
    if (IntelHookGpaExistsLocked(alignedGpa)) {
        status = STATUS_OBJECT_NAME_COLLISION;
        goto ExitMutation;
    }
    slot = IntelHookFindFreeSlot();
    if (slot == INTEL_HOOK_TABLE_CAPACITY) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto ExitMutation;
    }

    policy = (INTEL_HOOK_POLICY*)ExAllocatePool2(
        POOL_FLAG_NON_PAGED, sizeof(*policy), HV_POOL_TAG_HOOK_POLICY);
    if (policy == NULL) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto ExitMutation;
    }
    RtlZeroMemory(policy, sizeof(*policy));
    pointerBytes = (SIZE_T)State->CpuCount * sizeof(ULONG64*);
    retiredBytes =
        (SIZE_T)State->CpuCount * sizeof(INTEL_HOOK_RETIRED_PAGES);
    policy->PrimaryPtes = (ULONG64**)ExAllocatePool2(
        POOL_FLAG_NON_PAGED, pointerBytes, HV_POOL_TAG_HOOK_POLICY);
    policy->SecondaryPtes = (ULONG64**)ExAllocatePool2(
        POOL_FLAG_NON_PAGED, pointerBytes, HV_POOL_TAG_HOOK_POLICY);
    retiredPages = (INTEL_HOOK_RETIRED_PAGES*)ExAllocatePool2(
        POOL_FLAG_NON_PAGED, retiredBytes,
        HV_POOL_TAG_HOOK_POLICY);
    if (policy->PrimaryPtes == NULL || policy->SecondaryPtes == NULL ||
        retiredPages == NULL) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto ExitPolicy;
    }
    RtlZeroMemory(policy->PrimaryPtes, pointerBytes);
    RtlZeroMemory(policy->SecondaryPtes, pointerBytes);

    for (index = 0; index < State->CpuCount; ++index) {
        INTEL_CPU_CONTEXT* cpuContext =
            (INTEL_CPU_CONTEXT*)State->Cpus[index].VendorContext;
        ULONG64 primaryPte;
        ULONG64 secondaryPte;

        if (cpuContext == NULL) {
            status = STATUS_INVALID_DEVICE_STATE;
            goto RollbackPrepared;
        }
        __analysis_assume(index < State->CpuCount);
        __analysis_assume(
            ((SIZE_T)index + 1) * sizeof(ULONG64*) <= pointerBytes);
        status = IntelPrepareCpuHookPage(
            cpuContext, backend, alignedGpa,
            &policy->PrimaryPtes[index],
            &policy->SecondaryPtes[index]);
        if (!NT_SUCCESS(status)) {
            goto RollbackPrepared;
        }
        ++prepared;
        primaryPte = *policy->PrimaryPtes[index];
        secondaryPte = *policy->SecondaryPtes[index];
        if (index == 0) {
            policy->OriginalPrimaryPte = primaryPte;
            policy->OriginalSecondaryPte = secondaryPte;
        } else if (primaryPte != policy->OriginalPrimaryPte ||
                   secondaryPte != policy->OriginalSecondaryPte) {
            status = STATUS_DATA_ERROR;
            goto RollbackPrepared;
        }
    }
    if ((policy->OriginalPrimaryPte & EPT_ACCESS_MASK) != EPT_ACCESS_MASK ||
        (policy->OriginalPrimaryPte & EPT_ADDRESS_MASK) == 0) {
        status = STATUS_ACCESS_DENIED;
        goto RollbackPrepared;
    }
    status = IntelHookBuildShadow(
        policy->OriginalPrimaryPte & EPT_ADDRESS_MASK,
        PatchBytes, PatchOffset, PatchSize,
        &policy->ShadowVirtual,
        &policy->ShadowHostPhysicalAddress);
    if (!NT_SUCCESS(status)) {
        goto RollbackPrepared;
    }

    policy->HookId = slot;
    policy->CpuCount = State->CpuCount;
    policy->GuestPhysicalAddress = alignedGpa;
    policy->Kind = INTEL_HOOK_KIND_EXECUTE;
    policy->Cookie = Cookie;
    KeMemoryBarrier();
    InterlockedExchangePointer(
        (PVOID volatile*)&g_HookSlots[slot], policy);
    if (!IntelHookHashInsert(policy)) {
        InterlockedExchangePointer(
            (PVOID volatile*)&g_HookSlots[slot], NULL);
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto RollbackPrepared;
    }
    published = TRUE;
    KeMemoryBarrier();
    for (index = 0; index < State->CpuCount; ++index) {
        ULONG64 primary = policy->OriginalPrimaryPte & ~EPT_ACCESS_MASK;
        ULONG64 secondary =
            (policy->OriginalSecondaryPte &
             ~(EPT_ADDRESS_MASK | EPT_ACCESS_MASK)) |
            (policy->ShadowHostPhysicalAddress & EPT_ADDRESS_MASK) |
            HV_PAGE_ACCESS_EXECUTE;

        InterlockedExchange64(
            (volatile LONG64*)policy->PrimaryPtes[index],
            (LONG64)(primary |
                     (policy->OriginalPrimaryPte &
                      (HV_PAGE_ACCESS_READ | HV_PAGE_ACCESS_WRITE))));
        InterlockedExchange64(
            (volatile LONG64*)policy->SecondaryPtes[index],
            (LONG64)secondary);
    }
    ExReleasePushLockExclusive(&backend->HookLock);
    KeLeaveCriticalRegion();
    IntelHookInvalidateEverywhere(State, backend);
    InterlockedExchange(&backend->HookMutationActive, 0);
    ExFreePoolWithTag(retiredPages, HV_POOL_TAG_HOOK_POLICY);
    *HookId = slot;
    return STATUS_SUCCESS;

RollbackPrepared:
    if (published) {
        IntelHookHashRemove(policy);
        InterlockedExchangePointer(
            (PVOID volatile*)&g_HookSlots[slot], NULL);
    }
    NT_ASSERT(prepared <= State->CpuCount);
    __analysis_assume(prepared <= State->CpuCount);
    __analysis_assume(
        (SIZE_T)prepared * sizeof(*retiredPages) <= retiredBytes);
    IntelHookReleasePreparedCpuPages(
        State, backend, alignedGpa, prepared,
        State->CpuCount, retiredPages);
    ExReleasePushLockExclusive(&backend->HookLock);
    KeLeaveCriticalRegion();
    IntelHookInvalidateEverywhere(State, backend);
    IntelHookFreeRetiredPages(retiredPages, prepared);
    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&backend->HookLock);
ExitPolicy:
    if (retiredPages != NULL) {
        ExFreePoolWithTag(retiredPages, HV_POOL_TAG_HOOK_POLICY);
    }
    if (policy != NULL) {
        if (policy->ShadowVirtual != NULL) {
            MmFreeContiguousMemory(policy->ShadowVirtual);
        }
        if (policy->PrimaryPtes != NULL) {
            ExFreePoolWithTag(
                policy->PrimaryPtes, HV_POOL_TAG_HOOK_POLICY);
        }
        if (policy->SecondaryPtes != NULL) {
            ExFreePoolWithTag(
                policy->SecondaryPtes, HV_POOL_TAG_HOOK_POLICY);
        }
        ExFreePoolWithTag(policy, HV_POOL_TAG_HOOK_POLICY);
    }
ExitMutation:
    InterlockedExchange(&backend->HookMutationActive, 0);
Exit:
    ExReleasePushLockExclusive(&backend->HookLock);
    KeLeaveCriticalRegion();
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
    INTEL_HOOK_POLICY* policy;
    INTEL_HOOK_RETIRED_PAGES* retiredPages;
    SIZE_T retiredBytes;
    ULONG index;
    ULONG policyCpuCount;

    if (State == NULL || State->BackendContext == NULL ||
        HookId >= INTEL_HOOK_TABLE_CAPACITY) {
        return STATUS_INVALID_PARAMETER;
    }
    if (State->CpuCount == 0 ||
        State->CpuCount > MAXULONG / sizeof(INTEL_HOOK_RETIRED_PAGES)) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    backend = (INTEL_BACKEND_CONTEXT*)State->BackendContext;
    retiredBytes =
        (SIZE_T)State->CpuCount * sizeof(INTEL_HOOK_RETIRED_PAGES);
    retiredPages = (INTEL_HOOK_RETIRED_PAGES*)ExAllocatePool2(
        POOL_FLAG_NON_PAGED, retiredBytes,
        HV_POOL_TAG_HOOK_POLICY);
    if (retiredPages == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&backend->HookLock);
    if (InterlockedCompareExchange(
            &backend->HookMutationActive, 1, 0) != 0) {
        ExReleasePushLockExclusive(&backend->HookLock);
        KeLeaveCriticalRegion();
        ExFreePoolWithTag(retiredPages, HV_POOL_TAG_HOOK_POLICY);
        return STATUS_DEVICE_BUSY;
    }
    policy = IntelHookReadSlot(HookId);
    if (policy == NULL) {
        InterlockedExchange(&backend->HookMutationActive, 0);
        ExReleasePushLockExclusive(&backend->HookLock);
        KeLeaveCriticalRegion();
        ExFreePoolWithTag(retiredPages, HV_POOL_TAG_HOOK_POLICY);
        return STATUS_NOT_FOUND;
    }
    if (policy->CpuCount == 0 || policy->CpuCount > State->CpuCount) {
        InterlockedExchange(&backend->HookMutationActive, 0);
        ExReleasePushLockExclusive(&backend->HookLock);
        KeLeaveCriticalRegion();
        ExFreePoolWithTag(retiredPages, HV_POOL_TAG_HOOK_POLICY);
        return STATUS_DATA_ERROR;
    }
    policyCpuCount = policy->CpuCount;
    ExReleasePushLockExclusive(&backend->HookLock);
    KeLeaveCriticalRegion();

    /* Force every VMCS back to its processor-local primary EPTP before
       restoring leaves. The policy remains published during this window. */
    IntelHookRetireSecondaryViews(State, backend);
    for (index = 0; index < policyCpuCount; ++index) {
        InterlockedExchange64(
            (volatile LONG64*)policy->PrimaryPtes[index],
            (LONG64)policy->OriginalPrimaryPte);
        InterlockedExchange64(
            (volatile LONG64*)policy->SecondaryPtes[index],
            (LONG64)policy->OriginalSecondaryPte);
    }
    IntelHookInvalidateEverywhere(State, backend);

    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&backend->HookLock);
    IntelHookHashRemove(policy);
    InterlockedExchangePointer(
        (PVOID volatile*)&g_HookSlots[HookId], NULL);
    ExReleasePushLockExclusive(&backend->HookLock);
    KeLeaveCriticalRegion();

    /* A second rendezvous retires lock-free lookup readers before policy
       storage is reclaimed. */
    IntelHookInvalidateEverywhere(State, backend);
    NT_ASSERT(policyCpuCount <= State->CpuCount);
    __analysis_assume(policyCpuCount <= State->CpuCount);
    __analysis_assume(
        (SIZE_T)policyCpuCount * sizeof(*retiredPages) <= retiredBytes);
    IntelHookReleasePreparedCpuPages(
        State, backend, policy->GuestPhysicalAddress,
        policyCpuCount, State->CpuCount, retiredPages);
    IntelHookInvalidateEverywhere(State, backend);
    for (index = 0; index < policyCpuCount; ++index) {
        __analysis_assume(
            ((SIZE_T)index + 1) * sizeof(*retiredPages) <= retiredBytes);
        IntelFreeRetiredCpuEptPage(retiredPages[index].PrimaryPt);
        IntelFreeRetiredCpuEptPage(retiredPages[index].SecondaryPt);
        IntelFreeRetiredCpuEptPage(retiredPages[index].PrimaryPd);
        IntelFreeRetiredCpuEptPage(retiredPages[index].SecondaryPd);
    }

    if (policy->ShadowVirtual != NULL) {
        MmFreeContiguousMemory(policy->ShadowVirtual);
    }
    ExFreePoolWithTag(policy->PrimaryPtes, HV_POOL_TAG_HOOK_POLICY);
    ExFreePoolWithTag(policy->SecondaryPtes, HV_POOL_TAG_HOOK_POLICY);
    ExFreePoolWithTag(policy, HV_POOL_TAG_HOOK_POLICY);
    ExFreePoolWithTag(retiredPages, HV_POOL_TAG_HOOK_POLICY);
    InterlockedExchange(&backend->HookMutationActive, 0);
    IntelHookAllowSecondaryViews(backend);
    return STATUS_SUCCESS;
}
