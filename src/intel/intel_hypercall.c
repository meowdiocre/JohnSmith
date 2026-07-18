#include "intel_internal.h"
#include <ntddk.h>

#define FNV1A_OFFSET    0x811c9dc5u
#define FNV1A_PRIME     0x01000193u

static ULONG g_HypercallSeed;
static ULONG g_HypercallSubleaf;
static ULONG g_HypercallRegisterSubleaf;
static ULONG g_HypercallCommandIds[INTEL_HYPERCALL_CMD_COUNT];
static BOOLEAN g_HypercallSeeded;

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
JohnSmithQueryDword(
    _In_ PCWSTR ValueName,
    _Out_ PULONG Value
    );

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
JohnSmithDeleteRegistryValue(
    _In_ PCWSTR ValueName
    );

static ULONG
IntelFnv1a(
    _In_ ULONG Seed,
    _In_ PCSTR Tag
    )
{
    ULONG hash = Seed ^ FNV1A_OFFSET;
    while (*Tag != 0) {
        hash ^= (UCHAR)*Tag++;
        hash *= FNV1A_PRIME;
    }
    return hash;
}

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
IntelHypercallPrepare(
    VOID
    )
{
    ULONG i;
    ULONG seed;
    NTSTATUS status;
    UCHAR subTag[4];     /* "sub" */
    UCHAR srgTag[4];     /* "srg" */
    UCHAR regTag[4];     /* "reg" */
    UCHAR instTag[5];    /* "inst" */
    UCHAR remTag[4];     /* "rem" */
    UCHAR readTag[5];    /* "read" */
    UCHAR writTag[5];    /* "writ" */
    UCHAR querTag[5];    /* "quer" */
    UCHAR listTag[5];    /* "list" */
    UCHAR probTag[5];    /* "prob" */
    PCSTR tagPtrs[INTEL_HYPERCALL_CMD_COUNT];

    subTag[0]='s';subTag[1]='u';subTag[2]='b';subTag[3]=0;
    srgTag[0]='s';srgTag[1]='r';srgTag[2]='g';srgTag[3]=0;
    regTag[0]='r';regTag[1]='e';regTag[2]='g';regTag[3]=0;
    instTag[0]='i';instTag[1]='n';instTag[2]='s';instTag[3]='t';instTag[4]=0;
    remTag[0]='r';remTag[1]='e';remTag[2]='m';remTag[3]=0;
    readTag[0]='r';readTag[1]='e';readTag[2]='a';readTag[3]='d';readTag[4]=0;
    writTag[0]='w';writTag[1]='r';writTag[2]='i';writTag[3]='t';writTag[4]=0;
    querTag[0]='q';querTag[1]='u';querTag[2]='e';querTag[3]='r';querTag[4]=0;
    listTag[0]='l';listTag[1]='i';listTag[2]='s';listTag[3]='t';listTag[4]=0;
    probTag[0]='p';probTag[1]='r';probTag[2]='o';probTag[3]='b';probTag[4]=0;
    tagPtrs[0]=(PCSTR)regTag;
    tagPtrs[1]=(PCSTR)instTag;
    tagPtrs[2]=(PCSTR)remTag;
    tagPtrs[3]=(PCSTR)readTag;
    tagPtrs[4]=(PCSTR)writTag;
    tagPtrs[5]=(PCSTR)querTag;
    tagPtrs[6]=(PCSTR)listTag;
    tagPtrs[7]=(PCSTR)probTag;

    g_HypercallSeeded = FALSE;
    g_HypercallSeed = 0;
    g_HypercallSubleaf = 0;
    g_HypercallRegisterSubleaf = 0;
    RtlZeroMemory(g_HypercallCommandIds, sizeof(g_HypercallCommandIds));

    status = JohnSmithQueryDword(L"HypercallSeed", &seed);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    if (seed == 0) {
        return STATUS_INVALID_PARAMETER;
    }

    g_HypercallSeed = seed;
    g_HypercallSubleaf = IntelFnv1a(seed, (PCSTR)subTag);
    g_HypercallRegisterSubleaf = IntelFnv1a(seed, (PCSTR)srgTag);
    if (g_HypercallSubleaf == 0 ||
        g_HypercallRegisterSubleaf == 0 ||
        g_HypercallSubleaf == g_HypercallRegisterSubleaf) {
        g_HypercallSeed = 0;
        g_HypercallSubleaf = 0;
        g_HypercallRegisterSubleaf = 0;
        return STATUS_INVALID_PARAMETER;
    }

    for (i = 0; i < INTEL_HYPERCALL_CMD_COUNT; ++i) {
        g_HypercallCommandIds[i] = IntelFnv1a(seed, tagPtrs[i]);
    }

    status = JohnSmithDeleteRegistryValue(L"HypercallSeed");
    if (!NT_SUCCESS(status)) {
        g_HypercallSeed = 0;
        g_HypercallSubleaf = 0;
        g_HypercallRegisterSubleaf = 0;
        RtlZeroMemory(g_HypercallCommandIds, sizeof(g_HypercallCommandIds));
        return status;
    }
    g_HypercallSeeded = TRUE;
    return STATUS_SUCCESS;
}

BOOLEAN
IntelHypercallIsSeeded(
    VOID
    )
{
    return g_HypercallSeeded;
}

ULONG
IntelHypercallSeed(
    VOID
    )
{
    return g_HypercallSeed;
}

ULONG
IntelHypercallSubleaf(
    VOID
    )
{
    return g_HypercallSubleaf;
}

ULONG
IntelHypercallRegisterSubleaf(
    VOID
    )
{
    return g_HypercallRegisterSubleaf;
}

ULONG
IntelHypercallCommandId(
    _In_ INTEL_HYPERCALL_CMD Cmd
    )
{
    if ((ULONG)Cmd >= INTEL_HYPERCALL_CMD_COUNT) {
        return 0;
    }
    return g_HypercallCommandIds[Cmd];
}

/*
 * Intel SDM rev. 092, Vol. 3A §4.3 (4-level paging), §4.5 (large pages).
 *
 * PML4 (bits 47:39) -> PDPT (bits 38:30) -> PD (bits 29:21) -> PT
 * (bits 20:12).  PDPTE bit 7 selects a 1 GiB page; PDE bit 7 selects a
 * 2 MiB page.  Present is bit 0 at every level.  The PA mask strips the
 * low 12 bits and reserved high bits; large-page PFNs keep the low bits
 * of the VA inside the page region.
 */

#define X86_PTE_PRESENT     (1ull << 0)
#define X86_PTE_PFN_MASK    0x000FFFFFFFFFF000ull
#define X86_LARGE_PAGE_BIT  (1ull << 7)
#define X86_VA_SIGN_EXTEND  0xFFFF000000000000ull

static PVOID
IntelMapGuestPageTable(
    _In_ ULONG64 GuestPa
    )
{
    PHYSICAL_ADDRESS pa;
    pa.QuadPart = (LONGLONG)(GuestPa & X86_PTE_PFN_MASK);
    return MmGetVirtualForPhysical(pa);
}

static ULONG64
IntelSignExtendVa(
    _In_ ULONG64 Va
    )
{
    if ((Va & (1ull << 47)) != 0) {
        Va |= X86_VA_SIGN_EXTEND;
    }
    return Va;
}

_Success_(return == STATUS_SUCCESS)
NTSTATUS
IntelHypercallTranslateGuestVa(
    _In_ ULONG64 GuestCr3,
    _In_ ULONG64 GuestVa,
    _Out_ PULONG64 GuestPhysicalAddress,
    _Out_ PULONG64 PageSize,
    _In_ BOOLEAN Allow1GbPages
    )
{
    ULONG64 va;
    ULONG64 tablePa;
    ULONG64 entry;
    ULONG64 pfn;
    ULONG64 offset;
    PVOID table;

    if (GuestPhysicalAddress == NULL || PageSize == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    va = IntelSignExtendVa(GuestVa);
    tablePa = GuestCr3 & X86_PTE_PFN_MASK;

    /* PML4 */
    table = IntelMapGuestPageTable(tablePa);
    if (table == NULL) return STATUS_INVALID_PARAMETER;
    entry = ((volatile ULONG64*)table)[(va >> 39) & 0x1FF];
    if ((entry & X86_PTE_PRESENT) == 0) return STATUS_INVALID_PARAMETER;
    tablePa = entry & X86_PTE_PFN_MASK;

    /* PDPT */
    table = IntelMapGuestPageTable(tablePa);
    if (table == NULL) return STATUS_INVALID_PARAMETER;
    entry = ((volatile ULONG64*)table)[(va >> 30) & 0x1FF];
    if ((entry & X86_PTE_PRESENT) == 0) return STATUS_INVALID_PARAMETER;
    if ((entry & X86_LARGE_PAGE_BIT) != 0) {
        if (!Allow1GbPages) return STATUS_NOT_SUPPORTED;
        pfn = entry & 0x000FFFFFC0000000ull;
        offset = va & 0x3FFFFFFFull;
        *GuestPhysicalAddress = pfn | offset;
        *PageSize = 1024ull * 1024ull * 1024ull;
        return STATUS_SUCCESS;
    }
    tablePa = entry & X86_PTE_PFN_MASK;

    /* PD */
    table = IntelMapGuestPageTable(tablePa);
    if (table == NULL) return STATUS_INVALID_PARAMETER;
    entry = ((volatile ULONG64*)table)[(va >> 21) & 0x1FF];
    if ((entry & X86_PTE_PRESENT) == 0) return STATUS_INVALID_PARAMETER;
    if ((entry & X86_LARGE_PAGE_BIT) != 0) {
        pfn = entry & 0x000FFFFFFFE00000ull;
        offset = va & 0x1FFFFFull;
        *GuestPhysicalAddress = pfn | offset;
        *PageSize = 2ull * 1024ull * 1024ull;
        return STATUS_SUCCESS;
    }
    tablePa = entry & X86_PTE_PFN_MASK;

    /* PT */
    table = IntelMapGuestPageTable(tablePa);
    if (table == NULL) return STATUS_INVALID_PARAMETER;
    entry = ((volatile ULONG64*)table)[(va >> 12) & 0x1FF];
    if ((entry & X86_PTE_PRESENT) == 0) return STATUS_INVALID_PARAMETER;
    *GuestPhysicalAddress = (entry & X86_PTE_PFN_MASK) | (va & 0xFFFull);
    *PageSize = 4096ull;
    return STATUS_SUCCESS;
}
