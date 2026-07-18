#include "intel_internal.h"
#include "../common/x86_common.h"

VOID
IntelFlushEptIfNeeded(
    _Inout_ INTEL_CPU_CONTEXT* Context
    )
{
    INTEL_BACKEND_CONTEXT* backend =
        (INTEL_BACKEND_CONTEXT*)Context->BackendContext;
    INTEL_CPU_EPT_VIEW* activeView;
    LONG64 generation;
    INTEL_INVALIDATION_DESCRIPTOR descriptor;
    ULONG type;

    if (backend == NULL) {
        return;
    }
    /* Force-primary migration changes the active EPTRTA. The primary view is
       invalidated below only if its own generation is stale; the inactive
       secondary view keeps its generation until it is activated again. */
    if (InterlockedCompareExchange(&backend->ForcePrimaryEpt, 0, 0) != 0 &&
        Context->EptPointer != Context->PrimaryEpt.EptPointer) {
        if (!NT_SUCCESS(IntelVmWrite(
                VMCS_EPT_POINTER, Context->PrimaryEpt.EptPointer))) {
            KeBugCheckEx(HYPERVISOR_ERROR, INTEL_BUGCHECK_INVALIDATION,
                Context->EptPointer, Context->PrimaryEpt.EptPointer, 0);
        }
        Context->EptPointer = Context->PrimaryEpt.EptPointer;
    }

    if (Context->EptPointer == Context->PrimaryEpt.EptPointer) {
        activeView = &Context->PrimaryEpt;
    } else if (Context->EptPointer == Context->SecondaryEpt.EptPointer) {
        activeView = &Context->SecondaryEpt;
    } else {
        KeBugCheckEx(HYPERVISOR_ERROR, INTEL_BUGCHECK_INVALIDATION,
            Context->EptPointer, Context->PrimaryEpt.EptPointer,
            Context->SecondaryEpt.EptPointer);
    }

    generation = InterlockedCompareExchange64(
        &backend->SlatGeneration, 0, 0);
    if (InterlockedCompareExchange64(
            &activeView->SlatGeneration, 0, 0) == generation) {
        InterlockedExchange64(&Context->SlatGeneration, generation);
        return;
    }

    descriptor.Context = activeView->EptPointer;
    descriptor.Reserved = 0;
    type = (backend->EptVpidCapabilities & (1ull << 25)) != 0
        ? INVEPT_SINGLE_CONTEXT : INVEPT_ALL_CONTEXTS;
    if (type == INVEPT_ALL_CONTEXTS) {
        descriptor.Context = 0;
    }
    if (IntelAsmInvept(type, &descriptor) != 0) {
        KeBugCheckEx(HYPERVISOR_ERROR, INTEL_BUGCHECK_INVALIDATION,
            type, Context->EptPointer, generation);
    }
    if (type == INVEPT_ALL_CONTEXTS) {
        InterlockedExchange64(
            &Context->PrimaryEpt.SlatGeneration, generation);
        InterlockedExchange64(
            &Context->SecondaryEpt.SlatGeneration, generation);
    } else {
        InterlockedExchange64(&activeView->SlatGeneration, generation);
    }
    InterlockedExchange64(&Context->SlatGeneration, generation);
}

_Function_class_(KIPI_BROADCAST_WORKER)
_IRQL_requires_(IPI_LEVEL)
static ULONG_PTR
IntelSlatRendezvous(
    _In_ ULONG_PTR Argument
    )
{
    int registers[4];
    UNREFERENCED_PARAMETER(Argument);
    __cpuid(registers, 0);
    return 0;
}

static VOID
IntelInvalidateRunningSlat(
    _Inout_ HV_STATE* State,
    _Inout_ INTEL_BACKEND_CONTEXT* Backend
    )
{
    LONG64 generation;
    ULONG index;

    generation = InterlockedIncrement64(&Backend->SlatGeneration);
    KeIpiGenericCall(IntelSlatRendezvous, 0);
    for (index = 0; index < State->CpuCount; ++index) {
        INTEL_CPU_CONTEXT* cpuContext =
            (INTEL_CPU_CONTEXT*)State->Cpus[index].VendorContext;
        INTEL_CPU_EPT_VIEW* activeView = NULL;
        LONG64 cpuGeneration = 0;

        if (cpuContext != NULL) {
            if (cpuContext->EptPointer ==
                cpuContext->PrimaryEpt.EptPointer) {
                activeView = &cpuContext->PrimaryEpt;
            } else if (cpuContext->EptPointer ==
                       cpuContext->SecondaryEpt.EptPointer) {
                activeView = &cpuContext->SecondaryEpt;
            }
        }
        if (activeView != NULL) {
            cpuGeneration = InterlockedCompareExchange64(
                &activeView->SlatGeneration, 0, 0);
        }
        if (activeView == NULL || cpuGeneration != generation) {
            KeBugCheckEx(HYPERVISOR_ERROR, INTEL_BUGCHECK_INVALIDATION,
                index, generation, cpuGeneration);
        }
    }
}

VOID
IntelHookRetireSecondaryViews(
    _Inout_ HV_STATE* State,
    _Inout_ INTEL_BACKEND_CONTEXT* Backend
    )
{
    ULONG index;

    InterlockedExchange(&Backend->ForcePrimaryEpt, TRUE);
    if (InterlockedCompareExchange(&State->Lifecycle, 0, 0) !=
        HV_LIFECYCLE_RUNNING) {
        return;
    }

    IntelInvalidateRunningSlat(State, Backend);
    for (index = 0; index < State->CpuCount; ++index) {
        INTEL_CPU_CONTEXT* cpuContext =
            (INTEL_CPU_CONTEXT*)State->Cpus[index].VendorContext;
        if (cpuContext == NULL ||
            cpuContext->EptPointer != cpuContext->PrimaryEpt.EptPointer) {
            KeBugCheckEx(HYPERVISOR_ERROR, INTEL_BUGCHECK_INVALIDATION,
                index,
                cpuContext == NULL ? 0 : cpuContext->PrimaryEpt.EptPointer,
                cpuContext == NULL ? 0 : cpuContext->EptPointer);
        }
    }
}

VOID
IntelHookAllowSecondaryViews(
    _Inout_ INTEL_BACKEND_CONTEXT* Backend
    )
{
    InterlockedExchange(&Backend->ForcePrimaryEpt, FALSE);
}

_IRQL_requires_(PASSIVE_LEVEL)
VOID
IntelHookInvalidateEverywhere(
    _Inout_ HV_STATE* State,
    _Inout_ INTEL_BACKEND_CONTEXT* Backend
    )
{

    if (InterlockedCompareExchange(&State->Lifecycle, 0, 0) !=
        HV_LIFECYCLE_RUNNING) {
        return;
    }
    IntelInvalidateRunningSlat(State, Backend);
}

static INTEL_SLAT_SPLIT*
IntelFindSplit(
    _In_ INTEL_EPT_ROOT* Root,
    _In_ ULONG PdptIndex,
    _In_ ULONG PdIndex
    )
{
    PLIST_ENTRY entry;

    for (entry = Root->SplitList.Flink;
         entry != &Root->SplitList;
         entry = entry->Flink) {
        INTEL_SLAT_SPLIT* split = CONTAINING_RECORD(
            entry, INTEL_SLAT_SPLIT, Link);
        if (split->PdptIndex == PdptIndex && split->PdIndex == PdIndex) {
            return split;
        }
    }
    return NULL;
}

static PVOID
IntelFindSplitPt(
    _In_ INTEL_EPT_ROOT* Root,
    _In_ ULONG PdptIndex,
    _In_ ULONG PdIndex
    )
{
    INTEL_SLAT_SPLIT* split = IntelFindSplit(Root, PdptIndex, PdIndex);

    return split == NULL ? NULL : split->Pt;
}

/* Caller holds Root->SlatLock exclusively. */
_Success_(return != FALSE)
static BOOLEAN
IntelCanMerge2MbLocked(
    _In_ const ULONG64* Pt,
    _Out_ ULONG64* LargeEntry
    )
{
    ULONG64 attributes;
    ULONG64 base;
    ULONG index;

    *LargeEntry = 0;
    base = Pt[0] & EPT_ADDRESS_MASK;
    attributes = Pt[0] & (EPT_ACCESS_MASK | EPT_MEMORY_TYPE_MASK);
    if ((base & ((1ull << 21) - 1)) != 0) {
        return FALSE;
    }

    for (index = 0; index < 512; ++index) {
        ULONG64 expectedAddress = base + ((ULONG64)index << PAGE_SHIFT);
        ULONG64 entry = Pt[index];

        if ((entry & EPT_ADDRESS_MASK) != expectedAddress ||
            (entry & (EPT_ACCESS_MASK | EPT_MEMORY_TYPE_MASK)) != attributes ||
            (entry & ~(EPT_ADDRESS_MASK | EPT_ACCESS_MASK |
                       EPT_MEMORY_TYPE_MASK)) != 0) {
            return FALSE;
        }
    }

    *LargeEntry = (base & EPT_2MB_ADDRESS_MASK) |
                  attributes | EPT_LARGE_PAGE;
    return TRUE;
}

/* Caller holds Root->SlatLock exclusively. */
static INTEL_SLAT_SPLIT*
IntelTryMerge2MbLocked(
    _Inout_ INTEL_EPT_ROOT* Root,
    _In_ ULONG PdptIndex,
    _In_ ULONG PdIndex
    )
{
    INTEL_SLAT_SPLIT* split;
    ULONG64 largeEntry;
    ULONG64* pd;

    split = IntelFindSplit(Root, PdptIndex, PdIndex);

    if (split == NULL ||
        split->Reason != INTEL_SPLIT_REASON_PERMISSION ||
        !IntelCanMerge2MbLocked((const ULONG64*)split->Pt, &largeEntry)) {
        return NULL;
    }

    pd = (ULONG64*)Root->Pds[PdptIndex];
    RemoveEntryList(&split->Link);
    KeMemoryBarrier();
    InterlockedExchange64(
        (volatile LONG64*)&pd[PdIndex], (LONG64)largeEntry);
    return split;
}

static VOID
IntelFreeRetiredSplit(
    _In_opt_ INTEL_SLAT_SPLIT* Split
    )
{
    if (Split != NULL) {
        MmFreeContiguousMemory(Split->Pt);
        ExFreePoolWithTag(Split, HV_POOL_TAG_SLAT_SPLIT);
    }
}

static NTSTATUS
IntelSplit2MbLocked(
    _Inout_ INTEL_EPT_ROOT* Root,
    _In_ ULONG PdptIndex,
    _In_ ULONG PdIndex,
    _Outptr_ ULONG64** Pt
    )
{
    INTEL_SLAT_SPLIT* split;
    ULONG64* pd;
    ULONG64 pde;
    ULONG64 leafAttributes;
    ULONG index;

    *Pt = NULL;
    pd = (ULONG64*)Root->Pds[PdptIndex];
    pde = pd[PdIndex];
    if ((pde & EPT_LARGE_PAGE) == 0) {
        *Pt = (ULONG64*)IntelFindSplitPt(Root, PdptIndex, PdIndex);
        return *Pt == NULL ? STATUS_DATA_ERROR : STATUS_SUCCESS;
    }

    split = (INTEL_SLAT_SPLIT*)ExAllocatePool2(
        POOL_FLAG_NON_PAGED, sizeof(*split), HV_POOL_TAG_SLAT_SPLIT);
    if (split == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(split, sizeof(*split));
    split->Pt = IntelAllocatePage(MAXLONGLONG);
    if (split->Pt == NULL) {
        ExFreePoolWithTag(split, HV_POOL_TAG_SLAT_SPLIT);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    leafAttributes = pde & (EPT_ACCESS_MASK | EPT_MEMORY_TYPE_MASK);
    *Pt = (ULONG64*)split->Pt;
    for (index = 0; index < 512; ++index) {
        (*Pt)[index] = (pde & EPT_2MB_ADDRESS_MASK) |
                       ((ULONG64)index << PAGE_SHIFT) |
                       leafAttributes;
    }

    split->PdptIndex = PdptIndex;
    split->PdIndex = PdIndex;
    split->Reason = INTEL_SPLIT_REASON_PERMISSION;
    InsertTailList(&Root->SplitList, &split->Link);
    KeMemoryBarrier();
    InterlockedExchange64(
        (volatile LONG64*)&pd[PdIndex],
        (LONG64)(((ULONG64)MmGetPhysicalAddress(*Pt).QuadPart &
                  EPT_ADDRESS_MASK) | EPT_ACCESS_MASK));
    return STATUS_SUCCESS;
}

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
IntelHookAcquirePt(
    _Inout_ INTEL_EPT_ROOT* Root,
    _In_ ULONG64 MapLimit,
    _In_ ULONG64 GuestPhysicalAddress,
    _In_ INTEL_SPLIT_REASON NewSplitReason,
    _Outptr_ ULONG64** Pt,
    _Out_ ULONG* PtIndex
    )
{
    ULONG pdptIndex;
    ULONG pdIndex;
    NTSTATUS status;

    *Pt = NULL;
    *PtIndex = 0;
    if (GuestPhysicalAddress >= MapLimit ||
        GuestPhysicalAddress >= HV_SLAT_MAXIMUM_ADDRESS) {
        return STATUS_INVALID_ADDRESS;
    }

    pdptIndex = (ULONG)(GuestPhysicalAddress >> 30);
    pdIndex = (ULONG)((GuestPhysicalAddress >> 21) & 0x1ff);
    *PtIndex = (ULONG)((GuestPhysicalAddress >> 12) & 0x1ff);

    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&Root->SlatLock);
    status = IntelSplit2MbLocked(Root, pdptIndex, pdIndex, Pt);
    if (!NT_SUCCESS(status)) {
        ExReleasePushLockExclusive(&Root->SlatLock);
        KeLeaveCriticalRegion();
        return status;
    }

    if (NewSplitReason == INTEL_SPLIT_REASON_HOOK) {
        INTEL_SLAT_SPLIT* split = IntelFindSplit(Root, pdptIndex, pdIndex);
        if (split != NULL) {
            split->Reason = INTEL_SPLIT_REASON_HOOK;
        }
    }
    /* Lock is intentionally retained; caller releases via IntelHookReleasePt. */
    return STATUS_SUCCESS;
}

VOID
IntelHookReleasePt(
    _Inout_ INTEL_EPT_ROOT* Root
    )
{
    ExReleasePushLockExclusive(&Root->SlatLock);
    KeLeaveCriticalRegion();
}

PVOID
IntelAllocatePage(
    _In_ ULONG64 HighestAddress
    )
{
    PHYSICAL_ADDRESS low;
    PHYSICAL_ADDRESS high;
    PHYSICAL_ADDRESS boundary;
    PVOID page;

    low.QuadPart = 0;
    high.QuadPart = (LONGLONG)HighestAddress;
    boundary.QuadPart = 0;
    page = MmAllocateContiguousMemorySpecifyCache(
        PAGE_SIZE, low, high, boundary, MmCached);
    if (page != NULL) {
        RtlZeroMemory(page, PAGE_SIZE);
    }
    return page;
}

static NTSTATUS
IntelCreateMixedMapping(
    _Inout_ INTEL_EPT_ROOT* Root,
    _In_ PPHYSICAL_MEMORY_RANGE Ranges,
    _In_ ULONG PdptIndex,
    _In_ ULONG PdIndex,
    _In_ ULONG64 PhysicalAddress,
    _In_ ULONG64 LeafAccessBits,
    _Out_ ULONG64* Entry
    )
{
    INTEL_SLAT_SPLIT* split;
    ULONG64* pt;
    ULONG index;

    split = (INTEL_SLAT_SPLIT*)ExAllocatePool2(
        POOL_FLAG_NON_PAGED, sizeof(*split), HV_POOL_TAG_SLAT_SPLIT);
    if (split == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(split, sizeof(*split));
    split->Pt = IntelAllocatePage(MAXLONGLONG);
    if (split->Pt == NULL) {
        ExFreePoolWithTag(split, HV_POOL_TAG_SLAT_SPLIT);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    pt = (ULONG64*)split->Pt;
    for (index = 0; index < 512; ++index) {
        ULONG64 pageAddress = PhysicalAddress +
                              ((ULONG64)index << PAGE_SHIFT);
        ULONG64 memoryType = HvX86RangeIsRam(
            Ranges, pageAddress, PAGE_SIZE) ? EPT_MEMORY_TYPE_WB : 0;

        pt[index] = (pageAddress & EPT_ADDRESS_MASK) |
                    LeafAccessBits |
                    (memoryType << EPT_MEMORY_TYPE_SHIFT);
    }

    split->PdptIndex = PdptIndex;
    split->PdIndex = PdIndex;
    split->Reason = INTEL_SPLIT_REASON_MIXED_MEMORY_MAP;
    InsertTailList(&Root->SplitList, &split->Link);
    *Entry = ((ULONG64)MmGetPhysicalAddress(pt).QuadPart &
              EPT_ADDRESS_MASK) | EPT_ACCESS_MASK;
    return STATUS_SUCCESS;
}

NTSTATUS
IntelBuildIdentityRoot(
    _Inout_ INTEL_BACKEND_CONTEXT* Backend,
    _Inout_ INTEL_EPT_ROOT* Root,
    _In_ HV_PAGE_ACCESS LeafAccess
    )
{
    PPHYSICAL_MEMORY_RANGE ranges;
    ULONG pdptCount;
    ULONG pdptIndex;
    ULONG pdIndex;
    ULONG64* pml4;
    ULONG64* pdpt;
    ULONG64 leafAccessBits;

    leafAccessBits = ((ULONG64)LeafAccess) & EPT_ACCESS_MASK;
    if (leafAccessBits == 0) {
        return STATUS_INVALID_PARAMETER;
    }

    InitializeListHead(&Root->SplitList);
    /* SlatLock zero-initialization gives the correct initial pushlock state. */
    Root->Pml4 = IntelAllocatePage(MAXLONGLONG);
    Root->Pdpt = IntelAllocatePage(MAXLONGLONG);
    if (Root->Pml4 == NULL || Root->Pdpt == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    Root->OwnPml4 = TRUE;
    Root->OwnPdpt = TRUE;

    pml4 = (ULONG64*)Root->Pml4;
    pdpt = (ULONG64*)Root->Pdpt;
    pml4[0] = ((ULONG64)MmGetPhysicalAddress(Root->Pdpt).QuadPart &
               EPT_ADDRESS_MASK) | EPT_ACCESS_MASK;

    ranges = MmGetPhysicalMemoryRangesEx2(NULL, 0);
    if (ranges == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    pdptCount = (ULONG)((Backend->MapLimit + ((1ull << 30) - 1)) >> 30);
    for (pdptIndex = 0; pdptIndex < pdptCount; ++pdptIndex) {
        ULONG64* pd;

        Root->Pds[pdptIndex] = IntelAllocatePage(MAXLONGLONG);
        if (Root->Pds[pdptIndex] == NULL) {
            ExFreePool(ranges);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        Root->OwnPds[pdptIndex] = TRUE;

        pd = (ULONG64*)Root->Pds[pdptIndex];
        pdpt[pdptIndex] =
            ((ULONG64)MmGetPhysicalAddress(pd).QuadPart & EPT_ADDRESS_MASK) |
            EPT_ACCESS_MASK;

        for (pdIndex = 0; pdIndex < 512; ++pdIndex) {
            ULONG64 physical = ((ULONG64)pdptIndex << 30) |
                               ((ULONG64)pdIndex << 21);
            NTSTATUS status;

            if (physical >= Backend->MapLimit) {
                break;
            }
            if (HvX86RangeIsRam(ranges, physical, 1ull << 21)) {
                pd[pdIndex] = (physical & EPT_2MB_ADDRESS_MASK) |
                              leafAccessBits |
                              EPT_LARGE_PAGE |
                              (EPT_MEMORY_TYPE_WB <<
                               EPT_MEMORY_TYPE_SHIFT);
            } else if (HvX86RangeIntersectsRam(
                           ranges, physical, 1ull << 21)) {
                status = IntelCreateMixedMapping(
                    Root, ranges, pdptIndex, pdIndex,
                    physical, leafAccessBits, &pd[pdIndex]);
                if (!NT_SUCCESS(status)) {
                    ExFreePool(ranges);
                    return status;
                }
            } else {
                /* Non-RAM keeps identity mapping but with UC memory type;
                   permissions still follow the requested leaf mask so the
                   secondary root does not accidentally leave MMIO
                   executable to the guest. */
                pd[pdIndex] = (physical & EPT_2MB_ADDRESS_MASK) |
                              leafAccessBits |
                              EPT_LARGE_PAGE;
            }
        }
    }

    ExFreePool(ranges);

    Root->EptPointer = ((ULONG64)MmGetPhysicalAddress(Root->Pml4).QuadPart &
                        EPT_ADDRESS_MASK) |
                       EPT_MEMORY_TYPE_WB | (3ull << 3);
    return STATUS_SUCCESS;
}

NTSTATUS
IntelCloneRootTemplate(
    _In_ const INTEL_EPT_ROOT* Source,
    _Out_ INTEL_EPT_ROOT* Destination
    )
{
    ULONG64* pml4;
    ULONG index;

    if (Source == NULL || Destination == NULL || Source->Pml4 == NULL ||
        Source->Pdpt == NULL || Source->EptPointer == 0) {
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(Destination, sizeof(*Destination));
    InitializeListHead(&Destination->SplitList);
    Destination->Pml4 = IntelAllocatePage(MAXLONGLONG);
    Destination->Pdpt = IntelAllocatePage(MAXLONGLONG);
    if (Destination->Pml4 == NULL || Destination->Pdpt == NULL) {
        IntelFreeRoot(Destination);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    Destination->OwnPml4 = TRUE;
    Destination->OwnPdpt = TRUE;
    RtlCopyMemory(Destination->Pml4, Source->Pml4, PAGE_SIZE);
    RtlCopyMemory(Destination->Pdpt, Source->Pdpt, PAGE_SIZE);

    /* The project maps at most 512 GiB, so only PML4[0] is live.
       Add per-entry PDPT ownership when HV_SLAT_MAXIMUM_ADDRESS grows. */
    pml4 = (ULONG64*)Destination->Pml4;
    pml4[0] = ((ULONG64)MmGetPhysicalAddress(
                    Destination->Pdpt).QuadPart & EPT_ADDRESS_MASK) |
              (((const ULONG64*)Source->Pml4)[0] & ~EPT_ADDRESS_MASK);
    for (index = 0; index < RTL_NUMBER_OF(Destination->Pds); ++index) {
        Destination->Pds[index] = Source->Pds[index];
    }
    Destination->EptPointer =
        ((ULONG64)MmGetPhysicalAddress(Destination->Pml4).QuadPart &
         EPT_ADDRESS_MASK) |
        (Source->EptPointer & ~EPT_ADDRESS_MASK);
    return STATUS_SUCCESS;
}

VOID
IntelFreeRoot(
    _Inout_ INTEL_EPT_ROOT* Root
    )
{
    ULONG index;

    while (!IsListEmpty(&Root->SplitList)) {
        PLIST_ENTRY entry = RemoveHeadList(&Root->SplitList);
        INTEL_SLAT_SPLIT* split = CONTAINING_RECORD(
            entry, INTEL_SLAT_SPLIT, Link);

        if (split->Pt != NULL) {
            MmFreeContiguousMemory(split->Pt);
        }
        ExFreePoolWithTag(split, HV_POOL_TAG_SLAT_SPLIT);
    }

    for (index = 0; index < RTL_NUMBER_OF(Root->Pds); ++index) {
        if (Root->Pds[index] != NULL && Root->OwnPds[index]) {
            MmFreeContiguousMemory(Root->Pds[index]);
        }
        Root->Pds[index] = NULL;
        Root->OwnPds[index] = FALSE;
    }
    if (Root->Pdpt != NULL && Root->OwnPdpt) {
        MmFreeContiguousMemory(Root->Pdpt);
    }
    Root->Pdpt = NULL;
    Root->OwnPdpt = FALSE;
    if (Root->Pml4 != NULL && Root->OwnPml4) {
        MmFreeContiguousMemory(Root->Pml4);
    }
    Root->Pml4 = NULL;
    Root->OwnPml4 = FALSE;
    Root->EptPointer = 0;
}

NTSTATUS
IntelBuildEpt(
    _Inout_ INTEL_BACKEND_CONTEXT* Context
    )
{
    Context->MapLimit = HvX86GetSlatMapLimit();
    if (Context->MapLimit < (1ull << 32)) {
        return STATUS_HV_FEATURE_UNAVAILABLE;
    }

    RtlZeroMemory(&Context->PrimaryRoot, sizeof(Context->PrimaryRoot));
    return IntelBuildIdentityRoot(
        Context, &Context->PrimaryRoot,
        HV_PAGE_ACCESS_READ | HV_PAGE_ACCESS_WRITE | HV_PAGE_ACCESS_EXECUTE);
}

VOID
IntelFreeEpt(
    _Inout_ INTEL_BACKEND_CONTEXT* Context
    )
{
    if (Context->HookRoot != NULL) {
        IntelFreeRoot(Context->HookRoot);
        ExFreePoolWithTag(Context->HookRoot, HV_POOL_TAG_HOOK);
        Context->HookRoot = NULL;
    }
    IntelFreeRoot(&Context->PrimaryRoot);
}

static VOID
IntelFreeCpuEptView(
    _Inout_ INTEL_CPU_EPT_VIEW* View
    )
{
    ULONG index;

    while (!IsListEmpty(&View->SplitList)) {
        PLIST_ENTRY entry = RemoveHeadList(&View->SplitList);
        INTEL_CPU_EPT_SPLIT* split = CONTAINING_RECORD(
            entry, INTEL_CPU_EPT_SPLIT, Link);
        if (split->Pt != NULL) {
            MmFreeContiguousMemory(split->Pt);
        }
        ExFreePoolWithTag(split, HV_POOL_TAG_SLAT_SPLIT);
    }
    for (index = 0; index < RTL_NUMBER_OF(View->Pds); ++index) {
        if (View->Pds[index] != NULL) {
            MmFreeContiguousMemory(View->Pds[index]);
        }
    }
    if (View->Pdpt != NULL) {
        MmFreeContiguousMemory(View->Pdpt);
    }
    if (View->Pml4 != NULL) {
        MmFreeContiguousMemory(View->Pml4);
    }
    RtlZeroMemory(View, sizeof(*View));
}

static NTSTATUS
IntelInitializeCpuEptView(
    _Out_ INTEL_CPU_EPT_VIEW* View,
    _In_ const INTEL_EPT_ROOT* Template
    )
{
    ULONG64* pml4;

    RtlZeroMemory(View, sizeof(*View));
    InitializeListHead(&View->SplitList);
    View->Pml4 = IntelAllocatePage(MAXLONGLONG);
    View->Pdpt = IntelAllocatePage(MAXLONGLONG);
    if (View->Pml4 == NULL || View->Pdpt == NULL) {
        IntelFreeCpuEptView(View);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlCopyMemory(View->Pml4, Template->Pml4, PAGE_SIZE);
    RtlCopyMemory(View->Pdpt, Template->Pdpt, PAGE_SIZE);
    pml4 = (ULONG64*)View->Pml4;
    pml4[0] = ((ULONG64)MmGetPhysicalAddress(View->Pdpt).QuadPart &
               EPT_ADDRESS_MASK) |
              (((const ULONG64*)Template->Pml4)[0] & ~EPT_ADDRESS_MASK);
    /* Intel SDM rev. 092, Vol. 3C Table 27-9: WB=6, four-level
       walk is encoded as length-minus-one=3, root address in bits 51:12. */
    View->EptPointer =
        ((ULONG64)MmGetPhysicalAddress(View->Pml4).QuadPart &
         EPT_ADDRESS_MASK) |
        EPT_MEMORY_TYPE_WB | (3ull << 3);
    return STATUS_SUCCESS;
}

NTSTATUS
IntelInitializeCpuEptViews(
    _Inout_ INTEL_CPU_CONTEXT* Context,
    _In_ const INTEL_BACKEND_CONTEXT* Backend
    )
{
    NTSTATUS status;

    if (Context == NULL || Backend == NULL || Backend->HookRoot == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    status = IntelInitializeCpuEptView(
        &Context->PrimaryEpt, &Backend->PrimaryRoot);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    status = IntelInitializeCpuEptView(
        &Context->SecondaryEpt, Backend->HookRoot);
    if (!NT_SUCCESS(status)) {
        IntelFreeCpuEptView(&Context->PrimaryEpt);
        return status;
    }
    Context->PrimaryEpt.SlatGeneration = Backend->SlatGeneration;
    Context->SecondaryEpt.SlatGeneration = Backend->SlatGeneration;
    return STATUS_SUCCESS;
}

VOID
IntelFreeCpuEptViews(
    _Inout_ INTEL_CPU_CONTEXT* Context
    )
{
    if (Context == NULL) {
        return;
    }
    IntelFreeCpuEptView(&Context->SecondaryEpt);
    IntelFreeCpuEptView(&Context->PrimaryEpt);
}

static INTEL_CPU_EPT_SPLIT*
IntelFindCpuEptSplit(
    _In_ INTEL_CPU_EPT_VIEW* View,
    _In_ ULONG PdptIndex,
    _In_ ULONG PdIndex
    )
{
    PLIST_ENTRY entry;

    for (entry = View->SplitList.Flink;
         entry != &View->SplitList;
         entry = entry->Flink) {
        INTEL_CPU_EPT_SPLIT* split = CONTAINING_RECORD(
            entry, INTEL_CPU_EPT_SPLIT, Link);
        if (split->PdptIndex == PdptIndex && split->PdIndex == PdIndex) {
            return split;
        }
    }
    return NULL;
}

static PVOID
IntelVirtualForEptEntry(
    _In_ ULONG64 Entry
    )
{
    PHYSICAL_ADDRESS physical;

    physical.QuadPart = (LONGLONG)(Entry & EPT_ADDRESS_MASK);
    return MmGetVirtualForPhysical(physical);
}

static NTSTATUS
IntelPrepareCpuHookView(
    _Inout_ INTEL_CPU_EPT_VIEW* View,
    _In_ const INTEL_EPT_ROOT* Template,
    _In_ ULONG64 GuestPhysicalAddress,
    _Outptr_ ULONG64** Pte
    )
{
    ULONG pdptIndex = (ULONG)(GuestPhysicalAddress >> 30);
    ULONG pdIndex = (ULONG)((GuestPhysicalAddress >> 21) & 0x1ff);
    ULONG ptIndex = (ULONG)((GuestPhysicalAddress >> 12) & 0x1ff);
    INTEL_CPU_EPT_SPLIT* split;
    ULONG64* pd;

    *Pte = NULL;
    if (View->Pds[pdptIndex] == NULL) {
        PVOID privatePd = IntelAllocatePage(MAXLONGLONG);
        ULONG64 templateEntry;

        if (privatePd == NULL) {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        RtlCopyMemory(privatePd, Template->Pds[pdptIndex], PAGE_SIZE);
        templateEntry = ((const ULONG64*)Template->Pdpt)[pdptIndex];
        View->Pds[pdptIndex] = privatePd;
        KeMemoryBarrier();
        InterlockedExchange64(
            (volatile LONG64*)&((ULONG64*)View->Pdpt)[pdptIndex],
            (LONG64)(((ULONG64)MmGetPhysicalAddress(privatePd).QuadPart &
                      EPT_ADDRESS_MASK) |
                     (templateEntry & ~EPT_ADDRESS_MASK)));
    }

    pd = (ULONG64*)View->Pds[pdptIndex];
    split = IntelFindCpuEptSplit(View, pdptIndex, pdIndex);
    if (split == NULL) {
        ULONG64 sourcePde = pd[pdIndex];
        ULONG64* pt;
        ULONG index;

        split = (INTEL_CPU_EPT_SPLIT*)ExAllocatePool2(
            POOL_FLAG_NON_PAGED, sizeof(*split), HV_POOL_TAG_SLAT_SPLIT);
        if (split == NULL) {
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        RtlZeroMemory(split, sizeof(*split));
        split->Pt = IntelAllocatePage(MAXLONGLONG);
        if (split->Pt == NULL) {
            ExFreePoolWithTag(split, HV_POOL_TAG_SLAT_SPLIT);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        pt = (ULONG64*)split->Pt;
        if ((sourcePde & EPT_LARGE_PAGE) != 0) {
            ULONG64 attributes = sourcePde &
                (EPT_ACCESS_MASK | EPT_MEMORY_TYPE_MASK);
            for (index = 0; index < 512; ++index) {
                pt[index] = (sourcePde & EPT_2MB_ADDRESS_MASK) |
                            ((ULONG64)index << PAGE_SHIFT) | attributes;
            }
        } else {
            PVOID sourcePt = IntelVirtualForEptEntry(sourcePde);
            if (sourcePt == NULL) {
                MmFreeContiguousMemory(split->Pt);
                ExFreePoolWithTag(split, HV_POOL_TAG_SLAT_SPLIT);
                return STATUS_DATA_ERROR;
            }
            RtlCopyMemory(pt, sourcePt, PAGE_SIZE);
        }
        split->PdptIndex = pdptIndex;
        split->PdIndex = pdIndex;
        InsertTailList(&View->SplitList, &split->Link);
        KeMemoryBarrier();
        InterlockedExchange64(
            (volatile LONG64*)&pd[pdIndex],
            (LONG64)(((ULONG64)MmGetPhysicalAddress(pt).QuadPart &
                      EPT_ADDRESS_MASK) | EPT_ACCESS_MASK));
    }

    InterlockedIncrement(&split->ReferenceCount);
    ++View->PdHookCount[pdptIndex];
    *Pte = &((ULONG64*)split->Pt)[ptIndex];
    return STATUS_SUCCESS;
}

static VOID
IntelReleaseCpuHookView(
    _Inout_ INTEL_CPU_EPT_VIEW* View,
    _In_ const INTEL_EPT_ROOT* Template,
    _In_ ULONG64 GuestPhysicalAddress,
    _Outptr_result_maybenull_ PVOID* RetiredPt,
    _Outptr_result_maybenull_ PVOID* RetiredPd
    )
{
    ULONG pdptIndex = (ULONG)(GuestPhysicalAddress >> 30);
    ULONG pdIndex = (ULONG)((GuestPhysicalAddress >> 21) & 0x1ff);
    INTEL_CPU_EPT_SPLIT* split = IntelFindCpuEptSplit(
        View, pdptIndex, pdIndex);

    *RetiredPt = NULL;
    *RetiredPd = NULL;
    if (split == NULL || split->ReferenceCount <= 0) {
        return;
    }
    if (InterlockedDecrement(&split->ReferenceCount) == 0) {
        ULONG64 templatePde =
            ((const ULONG64*)Template->Pds[pdptIndex])[pdIndex];
        InterlockedExchange64(
            (volatile LONG64*)&((ULONG64*)View->Pds[pdptIndex])[pdIndex],
            (LONG64)templatePde);
        RemoveEntryList(&split->Link);
        *RetiredPt = split->Pt;
        split->Pt = NULL;
        ExFreePoolWithTag(split, HV_POOL_TAG_SLAT_SPLIT);
    }
    NT_ASSERT(View->PdHookCount[pdptIndex] != 0);
    if (--View->PdHookCount[pdptIndex] == 0) {
        ULONG64 templatePdpte =
            ((const ULONG64*)Template->Pdpt)[pdptIndex];
        PVOID pd = View->Pds[pdptIndex];
        InterlockedExchange64(
            (volatile LONG64*)&((ULONG64*)View->Pdpt)[pdptIndex],
            (LONG64)templatePdpte);
        View->Pds[pdptIndex] = NULL;
        *RetiredPd = pd;
    }
}

NTSTATUS
IntelPrepareCpuHookPage(
    _Inout_ INTEL_CPU_CONTEXT* Context,
    _In_ const INTEL_BACKEND_CONTEXT* Backend,
    _In_ ULONG64 GuestPhysicalAddress,
    _Outptr_ ULONG64** PrimaryPte,
    _Outptr_ ULONG64** SecondaryPte
    )
{
    NTSTATUS status;

    *PrimaryPte = NULL;
    *SecondaryPte = NULL;
    status = IntelPrepareCpuHookView(
        &Context->PrimaryEpt, &Backend->PrimaryRoot,
        GuestPhysicalAddress, PrimaryPte);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    status = IntelPrepareCpuHookView(
        &Context->SecondaryEpt, Backend->HookRoot,
        GuestPhysicalAddress, SecondaryPte);
    if (!NT_SUCCESS(status)) {
        PVOID retiredPt;
        PVOID retiredPd;
        IntelReleaseCpuHookView(
            &Context->PrimaryEpt, &Backend->PrimaryRoot,
            GuestPhysicalAddress, &retiredPt, &retiredPd);
        IntelFreeRetiredCpuEptPage(retiredPt);
        IntelFreeRetiredCpuEptPage(retiredPd);
    }
    return status;
}

VOID
IntelReleaseCpuHookPage(
    _Inout_ INTEL_CPU_CONTEXT* Context,
    _In_ const INTEL_BACKEND_CONTEXT* Backend,
    _In_ ULONG64 GuestPhysicalAddress,
    _Outptr_result_maybenull_ PVOID* RetiredPrimaryPt,
    _Outptr_result_maybenull_ PVOID* RetiredSecondaryPt,
    _Outptr_result_maybenull_ PVOID* RetiredPrimaryPd,
    _Outptr_result_maybenull_ PVOID* RetiredSecondaryPd
    )
{
    IntelReleaseCpuHookView(
        &Context->PrimaryEpt, &Backend->PrimaryRoot,
        GuestPhysicalAddress, RetiredPrimaryPt, RetiredPrimaryPd);
    IntelReleaseCpuHookView(
        &Context->SecondaryEpt, Backend->HookRoot,
        GuestPhysicalAddress, RetiredSecondaryPt, RetiredSecondaryPd);
}

VOID
IntelFreeRetiredCpuEptPage(
    _In_opt_ PVOID Page
    )
{
    if (Page != NULL) {
        MmFreeContiguousMemory(Page);
    }
}

NTSTATUS
IntelSwitchActiveEptRoot(
    _Inout_ INTEL_CPU_CONTEXT* Context,
    _In_ const INTEL_CPU_EPT_VIEW* View
    )
{
    INTEL_BACKEND_CONTEXT* backend;
    NTSTATUS status;

    if (Context == NULL || View == NULL || View->EptPointer == 0) {
        return STATUS_INVALID_PARAMETER;
    }
    backend = (INTEL_BACKEND_CONTEXT*)Context->BackendContext;
    if (backend != NULL &&
        InterlockedCompareExchange(&backend->ForcePrimaryEpt, 0, 0) != 0 &&
        View->EptPointer != Context->PrimaryEpt.EptPointer) {
        return STATUS_DEVICE_BUSY;
    }
    if (View->EptPointer == Context->EptPointer) {
        return STATUS_SUCCESS;
    }

    status = IntelVmWrite(VMCS_EPT_POINTER, View->EptPointer);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    Context->EptPointer = View->EptPointer;
    IntelFlushEptIfNeeded(Context);
    return STATUS_SUCCESS;
}

static NTSTATUS
IntelValidateOwnedAddress(
    _In_ INTEL_BACKEND_CONTEXT* Context,
    _In_ PHYSICAL_ADDRESS PhysicalAddress,
    _Out_ ULONG* PdptIndex,
    _Out_ ULONG* PdIndex,
    _Out_ ULONG* PtIndex
    )
{
    ULONG64 address = (ULONG64)PhysicalAddress.QuadPart;

    if (PhysicalAddress.QuadPart < 0) {
        return STATUS_INVALID_ADDRESS;
    }
    if (address >= Context->MapLimit ||
        address >= HV_SLAT_MAXIMUM_ADDRESS) {
        return STATUS_INVALID_ADDRESS;
    }

    *PdptIndex = (ULONG)(address >> 30);
    *PdIndex = (ULONG)((address >> 21) & 0x1ff);
    *PtIndex = (ULONG)((address >> 12) & 0x1ff);
    return STATUS_SUCCESS;
}

static BOOLEAN
IntelSlatMayChange(
    _In_ HV_STATE* State
    )
{
    LONG lifecycle = InterlockedCompareExchange(&State->Lifecycle, 0, 0);
    ULONG index;

    if (lifecycle == HV_LIFECYCLE_STARTING ||
        lifecycle == HV_LIFECYCLE_RUNNING) {
        return TRUE;
    }
    if (lifecycle != HV_LIFECYCLE_STOPPING) {
        return FALSE;
    }
    for (index = 0; index < State->CpuCount; ++index) {
        if (InterlockedCompareExchange(&State->Cpus[index].State, 0, 0) !=
            HV_CPU_PREPARED) {
            return FALSE;
        }
    }
    return TRUE;
}

NTSTATUS
IntelQueryOwnedPageAccess(
    _Inout_ HV_STATE* State,
    _In_ PHYSICAL_ADDRESS PhysicalAddress,
    _Out_ HV_PAGE_ACCESS* Access
    )
{
    INTEL_BACKEND_CONTEXT* context;
    INTEL_EPT_ROOT* root;
    ULONG pdptIndex;
    ULONG pdIndex;
    ULONG ptIndex;
    ULONG64 entry;
    NTSTATUS status;

    if (State == NULL || Access == NULL || State->BackendContext == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    context = (INTEL_BACKEND_CONTEXT*)State->BackendContext;
    root = &context->PrimaryRoot;
    status = IntelValidateOwnedAddress(
        context, PhysicalAddress, &pdptIndex, &pdIndex, &ptIndex);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    KeEnterCriticalRegion();
    ExAcquirePushLockShared(&root->SlatLock);
    entry = ((ULONG64*)root->Pds[pdptIndex])[pdIndex];
    if ((entry & EPT_LARGE_PAGE) == 0) {
        ULONG64* pt = (ULONG64*)IntelFindSplitPt(
            root, pdptIndex, pdIndex);
        if (pt == NULL) {
            status = STATUS_DATA_ERROR;
            goto Exit;
        }
        entry = pt[ptIndex];
    }
    *Access = (HV_PAGE_ACCESS)(entry & EPT_ACCESS_MASK);
    status = STATUS_SUCCESS;

Exit:
    ExReleasePushLockShared(&root->SlatLock);
    KeLeaveCriticalRegion();
    return status;
}

NTSTATUS
IntelSetOwnedPageAccess(
    _Inout_ HV_STATE* State,
    _In_ PHYSICAL_ADDRESS PhysicalAddress,
    _In_ HV_PAGE_ACCESS Access,
    _Out_ HV_PAGE_ACCESS* PreviousAccess
    )
{
    INTEL_BACKEND_CONTEXT* context;
    INTEL_EPT_ROOT* root;
    ULONG pdptIndex;
    ULONG pdIndex;
    ULONG ptIndex;
    ULONG64* pt;
    ULONG64 pte;
    NTSTATUS status;
    BOOLEAN invalidate = FALSE;
    INTEL_SLAT_SPLIT* retiredSplit = NULL;

    if (State == NULL || PreviousAccess == NULL ||
        State->BackendContext == NULL ||
        (((ULONG)Access) & ~EPT_ACCESS_MASK) != 0) {
        return STATUS_INVALID_PARAMETER;
    }
    if (!IntelSlatMayChange(State) || KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return STATUS_INVALID_DEVICE_STATE;
    }

    context = (INTEL_BACKEND_CONTEXT*)State->BackendContext;
    if ((((ULONG)Access) & HV_PAGE_ACCESS_WRITE) != 0 &&
        (((ULONG)Access) & HV_PAGE_ACCESS_READ) == 0) {
        return STATUS_NOT_SUPPORTED;
    }
    if ((((ULONG)Access) & HV_PAGE_ACCESS_EXECUTE) != 0 &&
        (((ULONG)Access) & HV_PAGE_ACCESS_READ) == 0 &&
        (context->EptVpidCapabilities & 1) == 0) {
        return STATUS_NOT_SUPPORTED;
    }
    status = IntelValidateOwnedAddress(
        context, PhysicalAddress, &pdptIndex, &pdIndex, &ptIndex);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    root = &context->PrimaryRoot;
    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&root->SlatLock);
    status = IntelSplit2MbLocked(
        root, pdptIndex, pdIndex, &pt);
    if (!NT_SUCCESS(status)) {
        goto Exit;
    }

    pte = pt[ptIndex];
    *PreviousAccess = (HV_PAGE_ACCESS)(pte & EPT_ACCESS_MASK);
    InterlockedExchange64(
        (volatile LONG64*)&pt[ptIndex],
        (LONG64)((pte & ~EPT_ACCESS_MASK) | (ULONG64)Access));
    KeMemoryBarrier();
    retiredSplit = IntelTryMerge2MbLocked(
        root, pdptIndex, pdIndex);
    invalidate = InterlockedCompareExchange(&State->Lifecycle, 0, 0) ==
        HV_LIFECYCLE_RUNNING;
    status = STATUS_SUCCESS;

Exit:
    ExReleasePushLockExclusive(&root->SlatLock);
    KeLeaveCriticalRegion();
    if (NT_SUCCESS(status) && invalidate) {
        IntelInvalidateRunningSlat(State, context);
    }
    /* The old PT cannot be reclaimed until every CPU discarded its walk. */
    if (NT_SUCCESS(status)) {
        IntelFreeRetiredSplit(retiredSplit);
    }
    return status;
}

NTSTATUS
IntelSetOwnedPageMapping(
    _Inout_ HV_STATE* State,
    _In_ PHYSICAL_ADDRESS GuestPhysicalAddress,
    _In_ PHYSICAL_ADDRESS HostPhysicalAddress,
    _In_ HV_PAGE_ACCESS Access,
    _Out_ PHYSICAL_ADDRESS* PreviousHostPhysicalAddress,
    _Out_ HV_PAGE_ACCESS* PreviousAccess
    )
{
    INTEL_BACKEND_CONTEXT* context;
    INTEL_EPT_ROOT* root;
    ULONG pdptIndex;
    ULONG pdIndex;
    ULONG ptIndex;
    ULONG64* pt;
    ULONG64 pte;
    NTSTATUS status;
    BOOLEAN invalidate = FALSE;
    INTEL_SLAT_SPLIT* retiredSplit = NULL;
    ULONG index;

    if (State == NULL || State->BackendContext == NULL ||
        PreviousHostPhysicalAddress == NULL || PreviousAccess == NULL ||
        HostPhysicalAddress.QuadPart < 0 ||
        (((ULONG64)HostPhysicalAddress.QuadPart & (PAGE_SIZE - 1)) != 0) ||
        (((ULONG)Access) & ~EPT_ACCESS_MASK) != 0) {
        return STATUS_INVALID_PARAMETER;
    }
    if (!IntelSlatMayChange(State) || KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return STATUS_INVALID_DEVICE_STATE;
    }

    context = (INTEL_BACKEND_CONTEXT*)State->BackendContext;
    if ((((ULONG)Access) & HV_PAGE_ACCESS_WRITE) != 0 &&
        (((ULONG)Access) & HV_PAGE_ACCESS_READ) == 0) {
        return STATUS_NOT_SUPPORTED;
    }
    if ((((ULONG)Access) & HV_PAGE_ACCESS_EXECUTE) != 0 &&
        (((ULONG)Access) & HV_PAGE_ACCESS_READ) == 0 &&
        (context->EptVpidCapabilities & 1) == 0) {
        return STATUS_NOT_SUPPORTED;
    }
    status = IntelValidateOwnedAddress(
        context, GuestPhysicalAddress, &pdptIndex, &pdIndex, &ptIndex);
    if (!NT_SUCCESS(status)) return status;
    if ((ULONG64)HostPhysicalAddress.QuadPart >= HV_SLAT_MAXIMUM_ADDRESS) {
        return STATUS_INVALID_ADDRESS;
    }

    root = &context->PrimaryRoot;
    if (context->HookRoot == NULL ||
        context->HookRoot->Pds[pdptIndex] != root->Pds[pdptIndex]) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    for (index = 0; index < State->CpuCount; ++index) {
        if (State->Cpus[index].VendorContext == NULL) {
            return STATUS_INVALID_DEVICE_STATE;
        }
    }

    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&context->HookLock);
    ExAcquirePushLockExclusive(&root->SlatLock);
    status = IntelSplit2MbLocked(root, pdptIndex, pdIndex, &pt);
    if (NT_SUCCESS(status)) {
        pte = pt[ptIndex];
        PreviousHostPhysicalAddress->QuadPart =
            (LONGLONG)(pte & EPT_ADDRESS_MASK);
        *PreviousAccess = (HV_PAGE_ACCESS)(pte & EPT_ACCESS_MASK);
        InterlockedExchange64(
            (volatile LONG64*)&pt[ptIndex],
            (LONG64)((pte & ~(EPT_ADDRESS_MASK | EPT_ACCESS_MASK)) |
                      ((ULONG64)HostPhysicalAddress.QuadPart & EPT_ADDRESS_MASK) |
                      (ULONG64)Access));
        KeMemoryBarrier();

        /* PrimaryRoot and HookRoot share template PD pages. Processor-local
           private PDs do not, so copy the changed leaf into both live views. */
        for (index = 0; index < State->CpuCount; ++index) {
            INTEL_CPU_CONTEXT* cpuContext =
                (INTEL_CPU_CONTEXT*)State->Cpus[index].VendorContext;
            INTEL_CPU_EPT_VIEW* views[2] = {
                &cpuContext->PrimaryEpt, &cpuContext->SecondaryEpt
            };
            ULONG viewIndex;

            for (viewIndex = 0; viewIndex < RTL_NUMBER_OF(views); ++viewIndex) {
                INTEL_CPU_EPT_VIEW* view = views[viewIndex];
                INTEL_CPU_EPT_SPLIT* split;
                ULONG64 templatePde;

                if (view->Pds[pdptIndex] == NULL) {
                    continue;
                }
                templatePde = ((ULONG64*)root->Pds[pdptIndex])[pdIndex];
                split = IntelFindCpuEptSplit(view, pdptIndex, pdIndex);
                if (split == NULL) {
                    InterlockedExchange64(
                        (volatile LONG64*)&
                            ((ULONG64*)view->Pds[pdptIndex])[pdIndex],
                        (LONG64)templatePde);
                } else {
                    ULONG64* templatePt =
                        (ULONG64*)IntelVirtualForEptEntry(templatePde);
                    if (templatePt == NULL) {
                        KeBugCheckEx(
                            HYPERVISOR_ERROR,
                            INTEL_BUGCHECK_INVALIDATION,
                            GuestPhysicalAddress.QuadPart,
                            templatePde,
                            index);
                    }
                    InterlockedExchange64(
                        (volatile LONG64*)&
                            ((ULONG64*)split->Pt)[ptIndex],
                        (LONG64)templatePt[ptIndex]);
                }
            }
        }

        retiredSplit = IntelTryMerge2MbLocked(
            root, pdptIndex, pdIndex);

        /* A merge replaces the shared PT with a 2 MiB leaf. Refresh private
           PDs before the retired template PT is freed. */
        if (retiredSplit != NULL) {
            ULONG64 templatePde =
                ((ULONG64*)root->Pds[pdptIndex])[pdIndex];
            ULONG64 templatePte =
                (templatePde & EPT_2MB_ADDRESS_MASK) |
                ((ULONG64)ptIndex << PAGE_SHIFT) |
                (templatePde & (EPT_ACCESS_MASK | EPT_MEMORY_TYPE_MASK));

            for (index = 0; index < State->CpuCount; ++index) {
                INTEL_CPU_CONTEXT* cpuContext =
                    (INTEL_CPU_CONTEXT*)State->Cpus[index].VendorContext;
                INTEL_CPU_EPT_VIEW* views[2] = {
                    &cpuContext->PrimaryEpt, &cpuContext->SecondaryEpt
                };
                ULONG viewIndex;

                for (viewIndex = 0;
                     viewIndex < RTL_NUMBER_OF(views);
                     ++viewIndex) {
                    INTEL_CPU_EPT_VIEW* view = views[viewIndex];
                    INTEL_CPU_EPT_SPLIT* split;

                    if (view->Pds[pdptIndex] == NULL) {
                        continue;
                    }
                    split = IntelFindCpuEptSplit(
                        view, pdptIndex, pdIndex);
                    if (split == NULL) {
                        InterlockedExchange64(
                            (volatile LONG64*)&
                                ((ULONG64*)view->Pds[pdptIndex])[pdIndex],
                            (LONG64)templatePde);
                    } else {
                        InterlockedExchange64(
                            (volatile LONG64*)&
                                ((ULONG64*)split->Pt)[ptIndex],
                            (LONG64)templatePte);
                    }
                }
            }
        }
        invalidate = InterlockedCompareExchange(
            &State->Lifecycle, 0, 0) == HV_LIFECYCLE_RUNNING;
    }
    ExReleasePushLockExclusive(&root->SlatLock);
    ExReleasePushLockExclusive(&context->HookLock);
    KeLeaveCriticalRegion();

    if (NT_SUCCESS(status) && invalidate) {
        IntelInvalidateRunningSlat(State, context);
    }
    if (NT_SUCCESS(status)) {
        IntelFreeRetiredSplit(retiredSplit);
    }
    return status;
}
