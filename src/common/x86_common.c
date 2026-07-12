#include "x86_common.h"

#include <intrin.h>

ULONG64
HvX86GetPhysicalAddressLimit(
    VOID
    )
{
    int registers[4];
    ULONG physicalAddressBits;

    __cpuid(registers, 0x80000000);
    if ((ULONG)registers[0] < 0x80000008u) {
        return 1ull << 36;
    }
    __cpuid(registers, 0x80000008);
    physicalAddressBits = (ULONG)registers[0] & 0xffu;
    return physicalAddressBits >= 63
        ? MAXLONGLONG
        : 1ull << physicalAddressBits;
}

ULONG64
HvX86GetSlatMapLimit(
    VOID
    )
{
    ULONG64 processorLimit = HvX86GetPhysicalAddressLimit();

    return processorLimit < HV_SLAT_MAXIMUM_ADDRESS
        ? processorLimit
        : HV_SLAT_MAXIMUM_ADDRESS;
}

BOOLEAN
HvX86RangeIsRam(
    _In_opt_ PPHYSICAL_MEMORY_RANGE Ranges,
    _In_ ULONG64 Base,
    _In_ ULONG64 Size
    )
{
    ULONG index;

    if (Ranges == NULL || Size == 0 || Base > MAXULONGLONG - Size) {
        return FALSE;
    }

    for (index = 0; Ranges[index].NumberOfBytes.QuadPart != 0; ++index) {
        ULONG64 rangeBase = (ULONG64)Ranges[index].BaseAddress.QuadPart;
        ULONG64 rangeSize = (ULONG64)Ranges[index].NumberOfBytes.QuadPart;
        ULONG64 offset;

        if (Base < rangeBase) {
            continue;
        }
        offset = Base - rangeBase;
        if (offset <= rangeSize && Size <= rangeSize - offset) {
            return TRUE;
        }
    }

    return FALSE;
}

BOOLEAN
HvX86RangeIntersectsRam(
    _In_opt_ PPHYSICAL_MEMORY_RANGE Ranges,
    _In_ ULONG64 Base,
    _In_ ULONG64 Size
    )
{
    ULONG index;
    ULONG64 end;

    if (Ranges == NULL || Size == 0 || Base > MAXULONGLONG - Size) {
        return FALSE;
    }
    end = Base + Size;

    for (index = 0; Ranges[index].NumberOfBytes.QuadPart != 0; ++index) {
        ULONG64 rangeBase = (ULONG64)Ranges[index].BaseAddress.QuadPart;
        ULONG64 rangeSize = (ULONG64)Ranges[index].NumberOfBytes.QuadPart;
        ULONG64 rangeEnd = rangeBase > MAXULONGLONG - rangeSize
            ? MAXULONGLONG
            : rangeBase + rangeSize;

        if (Base < rangeEnd && rangeBase < end) {
            return TRUE;
        }
    }

    return FALSE;
}
