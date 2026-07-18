#include <ntddk.h>
#include "../include/hook_observe.h"

#define HOOK_THUNK_SLOT_SIZE        24u
#define HOOK_THUNK_SLOTS_PER_PAGE   (PAGE_SIZE / HOOK_THUNK_SLOT_SIZE)
#define HOOK_THUNK_BITMAP_WORDS     ((HOOK_THUNK_SLOTS_PER_PAGE + 63u) / 64u)

C_ASSERT(HOOK_THUNK_SLOT_SIZE == 24);
C_ASSERT(HOOK_THUNK_SLOTS_PER_PAGE == 170);

typedef struct _HOOK_THUNK_PAGE {
    LIST_ENTRY Link;
    PVOID CodeBase;
    ULONG64 SlotBitmap[HOOK_THUNK_BITMAP_WORDS];
} HOOK_THUNK_PAGE;

typedef struct _HOOK_THUNK_STATE {
    LIST_ENTRY PageList;
    KSPIN_LOCK Lock;
    ULONG NextHookId;
} HOOK_THUNK_STATE;

static HOOK_THUNK_STATE g_ThunkState;

VOID
HookThunkInitialize(
    VOID
    )
{
    InitializeListHead(&g_ThunkState.PageList);
    KeInitializeSpinLock(&g_ThunkState.Lock);
    g_ThunkState.NextHookId = 1;
}

VOID
HookThunkReset(
    VOID
    )
{
    KIRQL oldIrql;
    LIST_ENTRY local;

    InitializeListHead(&local);
    KeAcquireSpinLock(&g_ThunkState.Lock, &oldIrql);
    if (!IsListEmpty(&g_ThunkState.PageList)) {
        local.Flink = g_ThunkState.PageList.Flink;
        local.Blink = g_ThunkState.PageList.Blink;
        local.Flink->Blink = &local;
        local.Blink->Flink = &local;
    }
    InitializeListHead(&g_ThunkState.PageList);
    g_ThunkState.NextHookId = 1;
    KeReleaseSpinLock(&g_ThunkState.Lock, oldIrql);

    while (!IsListEmpty(&local)) {
        PLIST_ENTRY entry = RemoveHeadList(&local);
        HOOK_THUNK_PAGE* page = CONTAINING_RECORD(
            entry, HOOK_THUNK_PAGE, Link);
        ExFreePoolWithTag(page->CodeBase, HV_POOL_TAG_HOOK_CODE);
        ExFreePoolWithTag(page, HV_POOL_TAG_HOOK_CODE);
    }
}

static ULONG
HookThunkFindFreeBit(
    _In_ const HOOK_THUNK_PAGE* Page
    )
{
    ULONG word;

    for (word = 0; word < HOOK_THUNK_BITMAP_WORDS; ++word) {
        ULONG64 bits = Page->SlotBitmap[word];
        ULONG bit;

        if (bits == MAXULONG64) {
            continue;
        }
        bit = 0;
        while (bit < 64) {
            ULONG64 mask = (ULONG64)1 << bit;
            ULONG slot;

            if ((bits & mask) == 0) {
                slot = word * 64 + bit;
                if (slot < HOOK_THUNK_SLOTS_PER_PAGE) {
                    return slot;
                }
                return HOOK_THUNK_SLOTS_PER_PAGE;
            }
            ++bit;
        }
    }
    return HOOK_THUNK_SLOTS_PER_PAGE;
}

static VOID
HookThunkSetBit(
    _Inout_ HOOK_THUNK_PAGE* Page,
    _In_ ULONG Slot
    )
{
    ULONG word = Slot / 64;
    ULONG bit = Slot % 64;
    Page->SlotBitmap[word] |= (ULONG64)1 << bit;
}

static VOID
HookThunkClearBit(
    _Inout_ HOOK_THUNK_PAGE* Page,
    _In_ ULONG Slot
    )
{
    ULONG word = Slot / 64;
    ULONG bit = Slot % 64;
    Page->SlotBitmap[word] &= ~((ULONG64)1 << bit);
}

NTSTATUS
HookThunkAllocate(
    _Out_ PVOID* SlotVirtual,
    _Out_ ULONG* HookId
    )
{
    KIRQL oldIrql;
    PLIST_ENTRY entry;
    HOOK_THUNK_PAGE* page;
    ULONG slot;
    ULONG id;

    *SlotVirtual = NULL;
    *HookId = 0;

    KeAcquireSpinLock(&g_ThunkState.Lock, &oldIrql);
    for (entry = g_ThunkState.PageList.Flink;
         entry != &g_ThunkState.PageList;
         entry = entry->Flink) {
        page = CONTAINING_RECORD(entry, HOOK_THUNK_PAGE, Link);
        slot = HookThunkFindFreeBit(page);
        if (slot < HOOK_THUNK_SLOTS_PER_PAGE) {
            HookThunkSetBit(page, slot);
            id = g_ThunkState.NextHookId++;
            KeReleaseSpinLock(&g_ThunkState.Lock, oldIrql);
            *SlotVirtual = (PUCHAR)page->CodeBase +
                (SIZE_T)slot * HOOK_THUNK_SLOT_SIZE;
            *HookId = id;
            return STATUS_SUCCESS;
        }
    }

    page = (HOOK_THUNK_PAGE*)ExAllocatePool2(
        POOL_FLAG_NON_PAGED, sizeof(*page), HV_POOL_TAG_HOOK_CODE);
    if (page == NULL) {
        KeReleaseSpinLock(&g_ThunkState.Lock, oldIrql);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    page->CodeBase = ExAllocatePool2(
        POOL_FLAG_NON_PAGED_EXECUTE | POOL_FLAG_UNINITIALIZED,
        PAGE_SIZE, HV_POOL_TAG_HOOK_CODE);
    if (page->CodeBase == NULL) {
        KeReleaseSpinLock(&g_ThunkState.Lock, oldIrql);
        ExFreePoolWithTag(page, HV_POOL_TAG_HOOK_CODE);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(page->SlotBitmap, sizeof(page->SlotBitmap));
    InsertHeadList(&g_ThunkState.PageList, &page->Link);

    slot = 0;
    HookThunkSetBit(page, slot);
    id = g_ThunkState.NextHookId++;
    KeReleaseSpinLock(&g_ThunkState.Lock, oldIrql);

    *SlotVirtual = (PUCHAR)page->CodeBase;
    *HookId = id;
    return STATUS_SUCCESS;
}

VOID
HookThunkFree(
    _In_ PVOID SlotVirtual
    )
{
    KIRQL oldIrql;
    PLIST_ENTRY entry;

    KeAcquireSpinLock(&g_ThunkState.Lock, &oldIrql);
    for (entry = g_ThunkState.PageList.Flink;
         entry != &g_ThunkState.PageList;
         entry = entry->Flink) {
        HOOK_THUNK_PAGE* page = CONTAINING_RECORD(
            entry, HOOK_THUNK_PAGE, Link);
        PUCHAR base = (PUCHAR)page->CodeBase;
        SIZE_T offset = (PUCHAR)SlotVirtual - base;

        if (offset < PAGE_SIZE &&
            offset % HOOK_THUNK_SLOT_SIZE == 0) {
            ULONG slot = (ULONG)(offset / HOOK_THUNK_SLOT_SIZE);
            HookThunkClearBit(page, slot);
            KeReleaseSpinLock(&g_ThunkState.Lock, oldIrql);
            return;
        }
    }
    KeReleaseSpinLock(&g_ThunkState.Lock, oldIrql);
}

ULONG
HookThunkSlotSize(
    VOID
    )
{
    return HOOK_THUNK_SLOT_SIZE;
}
