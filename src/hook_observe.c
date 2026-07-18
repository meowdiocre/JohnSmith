#include <ntddk.h>
#include "../include/hook_observe.h"
#include "../include/hv_log.h"
#include "../src/intel/intel_internal.h"

#define HOOK_OBSERVE_THUNK_SIZE 21u

/*
 * HookId encoding — 32 bits packed so dispatcher lookups are O(1).
 *
 *   bits  0..7  : observe slot (index into g_ObserveEntries, 0..63).
 *   bits 8..31  : per-boot thunk-allocation nonce (from HookThunkAllocate).
 *
 * ObserveHookRemove intentionally does not free the observe slot (the
 * "retired code stays allocated" invariant enforced by ObserveHookInstall),
 * so each slot is claimed at most once per hypervisor start. That
 * guarantees the encoded HookId uniquely identifies a specific
 * (slot, thunk) pair and cannot collide with any prior install at the same
 * slot within a single boot.
 */
#define HOOK_ID_SLOT_MASK           0xFFu
#define HOOK_ID_SLOT_BITS           8u

C_ASSERT(INTEL_HOOK_TABLE_CAPACITY <= (HOOK_ID_SLOT_MASK + 1u));

static PHOOK_OBSERVE_ENTRY volatile g_ObserveEntries[INTEL_HOOK_TABLE_CAPACITY];

static PHOOK_OBSERVE_ENTRY
ObserveEntryFromHookId(
    _In_ ULONG HookId
    )
{
    ULONG slot = HookId & HOOK_ID_SLOT_MASK;
    PHOOK_OBSERVE_ENTRY entry;

    if (slot >= INTEL_HOOK_TABLE_CAPACITY) {
        return NULL;
    }
    entry = (PHOOK_OBSERVE_ENTRY)InterlockedCompareExchangePointer(
        (PVOID volatile*)&g_ObserveEntries[slot], NULL, NULL);
    if (entry == NULL || entry->HookId != HookId) {
        return NULL;
    }
    return entry;
}

VOID
ObserveHookReset(
    VOID
    )
{
    ULONG index;

    for (index = 0; index < INTEL_HOOK_TABLE_CAPACITY; ++index) {
        PHOOK_OBSERVE_ENTRY entry = (PHOOK_OBSERVE_ENTRY)
            InterlockedExchangePointer(
                (PVOID volatile*)&g_ObserveEntries[index], NULL);
        if (entry == NULL) {
            continue;
        }
        HookTrampolineFree(entry->TrampolineVirtual);
        HookThunkFree(entry->ThunkVirtual);
        ExFreePoolWithTag(entry, HV_POOL_TAG_HOOK_META);
    }
    HookThunkReset();
}

static PHOOK_OBSERVE_ENTRY
ObserveReadEntry(
    _In_ ULONG Index
    )
{
    return (PHOOK_OBSERVE_ENTRY)InterlockedCompareExchangePointer(
        (PVOID volatile*)&g_ObserveEntries[Index], NULL, NULL);
}

/*
 * Atomically claim a free slot for Entry via CAS. On success, Entry is
 * published into g_ObserveEntries with Active still FALSE — concurrent
 * ObserveEntryFromHookId probes see the entry but reject it because its
 * HookId is still zero. The publisher fills the entry, memory-barriers,
 * and then flips Active. Failure paths must exchange NULL back before
 * freeing.
 */
static ULONG
ObserveReserveSlot(
    _In_ PHOOK_OBSERVE_ENTRY Entry
    )
{
    ULONG index;

    for (index = 0; index < INTEL_HOOK_TABLE_CAPACITY; ++index) {
        if (InterlockedCompareExchangePointer(
                (PVOID volatile*)&g_ObserveEntries[index],
                Entry,
                NULL) == NULL) {
            return index;
        }
    }
    return INTEL_HOOK_TABLE_CAPACITY;
}

static VOID
ObserveReleaseSlot(
    _In_ ULONG Index,
    _In_ PHOOK_OBSERVE_ENTRY Entry
    )
{
    (VOID)InterlockedCompareExchangePointer(
        (PVOID volatile*)&g_ObserveEntries[Index], NULL, Entry);
}

static NTSTATUS
ObserveEncodeThunk(
    _Out_writes_bytes_(HOOK_OBSERVE_THUNK_SIZE) PUCHAR Buffer,
    _In_ ULONG HookId
    )
{
    ULONG64 dispatcher = (ULONG64)&AsmHookDispatcher;

    /*
     * 41 52          : PUSH R10 so the dispatcher can restore it.
     * 68 <imm32>     : PUSH sign-extended HookId without losing an argument.
     * 49 BA <imm64>  : MOV R10, absolute VA of AsmHookDispatcher.
     * 41 FF E2       : JMP R10 without reading the execute-only shadow page.
     * 90             : pad the fixed patch window to 21 bytes.
     */
    Buffer[0] = 0x41;
    Buffer[1] = 0x52;
    Buffer[2] = 0x68;
    Buffer[3] = (UCHAR)(HookId & 0xFF);
    Buffer[4] = (UCHAR)((HookId >> 8) & 0xFF);
    Buffer[5] = (UCHAR)((HookId >> 16) & 0xFF);
    Buffer[6] = (UCHAR)((HookId >> 24) & 0xFF);
    Buffer[7] = 0x49;
    Buffer[8] = 0xBA;
    Buffer[9] = (UCHAR)(dispatcher & 0xFF);
    Buffer[10] = (UCHAR)((dispatcher >> 8) & 0xFF);
    Buffer[11] = (UCHAR)((dispatcher >> 16) & 0xFF);
    Buffer[12] = (UCHAR)((dispatcher >> 24) & 0xFF);
    Buffer[13] = (UCHAR)((dispatcher >> 32) & 0xFF);
    Buffer[14] = (UCHAR)((dispatcher >> 40) & 0xFF);
    Buffer[15] = (UCHAR)((dispatcher >> 48) & 0xFF);
    Buffer[16] = (UCHAR)((dispatcher >> 56) & 0xFF);
    Buffer[17] = 0x41;
    Buffer[18] = 0xFF;
    Buffer[19] = 0xE2;
    Buffer[20] = 0x90;
    return STATUS_SUCCESS;
}

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
ObserveHookInstall(
    _Inout_ HV_STATE* State,
    _In_ PVOID TargetVirtualAddress,
    _In_ ULONG64 Cookie,
    _In_ HOOK_OBSERVE_CALLBACK Callback,
    _Out_ ULONG* HookId
    )
{
    NTSTATUS status;
    PVOID thunkVa = NULL;
    ULONG thunkHookId = 0;
    ULONG intelSlot = INTEL_HOOK_TABLE_CAPACITY;
    ULONG observeSlot;
    PVOID trampolineVa = NULL;
    ULONG bytesCopied = 0;
    UCHAR thunkBytes[HOOK_OBSERVE_THUNK_SIZE];
    PHYSICAL_ADDRESS targetPa;
    ULONG64 targetGpa;
    ULONG pageOffset;
    PHOOK_OBSERVE_ENTRY entry = NULL;

    if (State == NULL || State->BackendContext == NULL ||
        TargetVirtualAddress == NULL || Callback == NULL || HookId == NULL ||
        (ULONG_PTR)TargetVirtualAddress <= (ULONG_PTR)MmHighestUserAddress) {
        return STATUS_INVALID_PARAMETER;
    }
    *HookId = INTEL_HOOK_TABLE_CAPACITY;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return STATUS_INVALID_DEVICE_STATE;
    }
    if (State->Backend->HookInstall == NULL ||
        State->Backend->HookRemove == NULL) {
        return STATUS_NOT_SUPPORTED;
    }
    pageOffset = (ULONG)((ULONG_PTR)TargetVirtualAddress & (PAGE_SIZE - 1));
    if ((ULONG64)pageOffset + HOOK_OBSERVE_THUNK_SIZE > PAGE_SIZE) {
        return STATUS_INVALID_ADDRESS;
    }

    status = HookThunkAllocate(&thunkVa, &thunkHookId);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = HookTrampolineBuild(
        (PUCHAR)TargetVirtualAddress, HOOK_OBSERVE_THUNK_SIZE,
        &trampolineVa, &bytesCopied);
    if (!NT_SUCCESS(status)) {
        goto FreeThunk;
    }

    /* thunkBytes is encoded below, after the observe slot is reserved and
       the packed HookId is known. */

    targetPa = MmGetPhysicalAddress(TargetVirtualAddress);
    targetGpa = (ULONG64)targetPa.QuadPart;
    if (targetGpa == 0 || (targetGpa & (PAGE_SIZE - 1)) !=
        ((ULONG64)(ULONG_PTR)TargetVirtualAddress & (PAGE_SIZE - 1))) {
        status = STATUS_INVALID_ADDRESS;
        goto FreeTrampoline;
    }

    /* Reserve the thunk nonce space so we can encode the packed HookId. A
       24-bit nonce plus 8-bit slot fits comfortably: even at one install per
       nanosecond the nonce takes ~16 ms to exhaust, and real workloads cap at
       INTEL_HOOK_TABLE_CAPACITY (64) per boot. */
    if (thunkHookId >= (1u << (32u - HOOK_ID_SLOT_BITS))) {
        status = STATUS_INTEGER_OVERFLOW;
        goto FreeTrampoline;
    }

    entry = (PHOOK_OBSERVE_ENTRY)ExAllocatePool2(
        POOL_FLAG_NON_PAGED, sizeof(*entry), HV_POOL_TAG_HOOK_META);
    if (entry == NULL) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto FreeTrampoline;
    }
    RtlZeroMemory(entry, sizeof(*entry));
    entry->BackendHookId = INTEL_HOOK_TABLE_CAPACITY;
    entry->GuestPhysicalAddress = targetGpa & ~((ULONG64)PAGE_SIZE - 1);
    entry->Cookie = Cookie;
    entry->ThunkVirtual = thunkVa;
    entry->TrampolineVirtual = trampolineVa;
    entry->Callback = Callback;
    entry->HitCount = 0;

    /* Publish the entry pointer first — it becomes visible to dispatcher
       lookups with HookId == 0, and ObserveEntryFromHookId rejects those
       because the encoded HookId cannot equal zero (the thunk nonce starts
       at 1). Once the slot is claimed, the entry's HookId field is set with
       release semantics before the first invocation is possible. */
    observeSlot = ObserveReserveSlot(entry);
    if (observeSlot == INTEL_HOOK_TABLE_CAPACITY) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto FreeEntry;
    }

    /* Pack the final HookId. The slot index becomes the direct-lookup key
       for the dispatcher hot path. */
    {
        ULONG packedId =
            (thunkHookId << HOOK_ID_SLOT_BITS) | (observeSlot & HOOK_ID_SLOT_MASK);

        entry->HookId = packedId;
        KeMemoryBarrier();
        InterlockedExchange(&entry->Active, TRUE);

        status = ObserveEncodeThunk(thunkBytes, packedId);
        if (!NT_SUCCESS(status)) {
            ObserveReleaseSlot(observeSlot, entry);
            goto FreeEntry;
        }

        status = State->Backend->HookInstall(
            State, targetGpa, thunkBytes, pageOffset, HOOK_OBSERVE_THUNK_SIZE,
            Cookie, &intelSlot);
        if (!NT_SUCCESS(status)) {
            ObserveReleaseSlot(observeSlot, entry);
            goto FreeEntry;
        }
        entry->BackendHookId = intelSlot;
        KeMemoryBarrier();

        *HookId = packedId;
    }
    return STATUS_SUCCESS;

FreeEntry:
    ExFreePoolWithTag(entry, HV_POOL_TAG_HOOK_META);
FreeTrampoline:
    HookTrampolineFree(trampolineVa);
FreeThunk:
    HookThunkFree(thunkVa);
    return status;
}

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
ObserveHookRemove(
    _Inout_ HV_STATE* State,
    _In_ ULONG HookId
    )
{
    PHOOK_OBSERVE_ENTRY entry;
    NTSTATUS status;

    if (State == NULL || HookId == 0) {
        return STATUS_INVALID_PARAMETER;
    }
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return STATUS_INVALID_DEVICE_STATE;
    }

    entry = ObserveEntryFromHookId(HookId);
    if (entry == NULL ||
        InterlockedCompareExchange(&entry->Active, 0, 0) == 0) {
        return STATUS_NOT_FOUND;
    }

    status = State->Backend->HookRemove(State, entry->BackendHookId);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    InterlockedExchange(&entry->Active, FALSE);
    /* ponytail: retired code stays allocated until hypervisor stop; this
       caps installs to 64 per start and avoids cross-CPU executable UAF. */
    return STATUS_SUCCESS;
}

NTSTATUS
ObserveHookQuery(
    _In_ ULONG HookId,
    _Out_ PHOOK_OBSERVE_QUERY_ENTRY Query
    )
{
    PHOOK_OBSERVE_ENTRY entry;

    if (Query == NULL || HookId == 0) {
        return STATUS_INVALID_PARAMETER;
    }
    RtlZeroMemory(Query, sizeof(*Query));

    entry = ObserveEntryFromHookId(HookId);
    if (entry == NULL ||
        InterlockedCompareExchange(&entry->Active, 0, 0) == 0) {
        return STATUS_NOT_FOUND;
    }
    Query->HookId = entry->HookId;
    Query->Active = 1;
    Query->GuestPhysicalAddress = entry->GuestPhysicalAddress;
    Query->Cookie = entry->Cookie;
    Query->HitCount = InterlockedCompareExchange64(
        &entry->HitCount, 0, 0);
    return STATUS_SUCCESS;
}

ULONG
ObserveHookList(
    _Out_writes_to_(Capacity, return) PHOOK_OBSERVE_QUERY_ENTRY Queries,
    _In_ ULONG Capacity
    )
{
    ULONG count = 0;
    ULONG index;

    if (Queries == NULL || Capacity == 0) {
        return 0;
    }
    for (index = 0;
         index < INTEL_HOOK_TABLE_CAPACITY && count < Capacity;
         ++index) {
        PHOOK_OBSERVE_ENTRY entry = ObserveReadEntry(index);
        if (entry == NULL ||
            InterlockedCompareExchange(&entry->Active, 0, 0) == 0) {
            continue;
        }
        Queries[count].HookId = entry->HookId;
        Queries[count].Active = 1;
        Queries[count].GuestPhysicalAddress = entry->GuestPhysicalAddress;
        Queries[count].Cookie = entry->Cookie;
        Queries[count].HitCount = InterlockedCompareExchange64(
            &entry->HitCount, 0, 0);
        ++count;
    }
    return count;
}

PVOID
HookObserveDispatch(
    _In_ ULONG HookId,
    _In_ ULONG64 CallerReturnAddress
    )
{
    /*
     * Dispatcher hot path — one O(1) lookup supplies the hit counter,
     * callback, and trampoline. The entry remains allocated after removal,
     * so an invocation already in flight can safely continue.
     */
    PHOOK_OBSERVE_ENTRY entry = ObserveEntryFromHookId(HookId);

    if (entry == NULL) {
        /* Unknown HookId: the hook may be mid-teardown. Drop silently. */
        return NULL;
    }
    InterlockedIncrement64(&entry->HitCount);
    entry->Callback(HookId, CallerReturnAddress);
    return entry->TrampolineVirtual;
}
