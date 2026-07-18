#include <ntddk.h>
#include "../include/hook_observe.h"

#define HOOK_REL32_MIN (-2147483647LL - 1LL)
#define HOOK_REL32_MAX 2147483647LL

#define HOOK_TRAMPOLINE_MAX_ORIGINAL   32u
#define HOOK_TRAMPOLINE_JMP_SIZE       14u
#define HOOK_TRAMPOLINE_ALLOC          (HOOK_TRAMPOLINE_MAX_ORIGINAL + \
                                        HOOK_TRAMPOLINE_JMP_SIZE)

typedef struct _HOOK_DECODER {
    const UCHAR* Start;
    ULONG Offset;
    ULONG Length;
} HOOK_DECODER;

static UCHAR HookDecPeek(const HOOK_DECODER* Dec) { return Dec->Start[Dec->Offset]; }
static UCHAR HookDecRead(HOOK_DECODER* Dec) { UCHAR v = Dec->Start[Dec->Offset]; ++Dec->Offset; return v; }
static BOOLEAN HookDecHasBytes(const HOOK_DECODER* Dec, ULONG Count) { return Dec->Offset + Count <= Dec->Length; }

typedef struct _HOOK_INSTR_INFO {
    ULONG Length;
    BOOLEAN RipRelative;
    ULONG Disp32Offset;     /* offset within instruction of the 4-byte disp */
} HOOK_INSTR_INFO;

static BOOLEAN
HookDecConsumeModRmTail(
    _Inout_ HOOK_DECODER* Dec,
    _In_ UCHAR ModRm,
    _In_ ULONG InstrStartOffset,
    _Out_ BOOLEAN* RipRelative,
    _Out_ ULONG* Disp32InstrOffset
    )
{
    UCHAR mod = (UCHAR)(ModRm >> 6);
    UCHAR rm = (UCHAR)(ModRm & 7);

    *RipRelative = FALSE;
    *Disp32InstrOffset = 0;

    if (mod == 3) {
        return TRUE;                       /* register-direct */
    }

    if (rm == 4) {
        UCHAR sib;
        UCHAR base;
        if (!HookDecHasBytes(Dec, 1)) {
            return FALSE;
        }
        sib = HookDecRead(Dec);
        base = (UCHAR)(sib & 7);
        if (mod == 0 && base == 5) {
            /* SIB with base==101 and mod==00 hides a disp32 (SDM Vol 2A). */
            if (!HookDecHasBytes(Dec, 4)) {
                return FALSE;
            }
            Dec->Offset += 4;
        }
    } else if (mod == 0 && rm == 5) {
        /* RIP-relative addressing. */
        *RipRelative = TRUE;
        *Disp32InstrOffset = Dec->Offset - InstrStartOffset;
        if (!HookDecHasBytes(Dec, 4)) {
            return FALSE;
        }
        Dec->Offset += 4;
        return TRUE;
    }

    if (mod == 1) {
        if (!HookDecHasBytes(Dec, 1)) {
            return FALSE;
        }
        Dec->Offset += 1;
    } else if (mod == 2) {
        if (!HookDecHasBytes(Dec, 4)) {
            return FALSE;
        }
        Dec->Offset += 4;
    }

    return TRUE;
}

static ULONG
HookDecImmediateSize(
    _In_ UCHAR Opcode,
    _In_ UCHAR RexW,
    _In_ UCHAR OperandSizeOverride,
    _In_ UCHAR ModRmRegField    /* the /digit, valid only for F6/F7/C0/C1 */
    )
{
    switch (Opcode) {
    case 0xB0: case 0xB1: case 0xB2: case 0xB3:
    case 0xB4: case 0xB5: case 0xB6: case 0xB7:
        return 1;                              /* MOV r8, imm8 */
    case 0xB8: case 0xB9: case 0xBA: case 0xBB:
    case 0xBC: case 0xBD: case 0xBE: case 0xBF:
        return RexW ? 8 : (OperandSizeOverride ? 2 : 4); /* MOV r, imm */
    case 0x68:                               /* PUSH imm32 */
        return OperandSizeOverride ? 2 : 4;
    case 0x69:                               /* IMUL r, r/m, imm32 */
        return RexW ? 4 : (OperandSizeOverride ? 2 : 4);
    case 0x6B:                               /* IMUL r, r/m, imm8 */
        return 1;
    case 0xA9:                               /* TEST EAX, imm32 */
        return RexW ? 4 : (OperandSizeOverride ? 2 : 4);
    case 0xC7:                               /* MOV r/m, imm32 */
        return RexW ? 4 : (OperandSizeOverride ? 2 : 4);
    case 0x81:                               /* arithmetic r/m, imm32 */
        return RexW ? 4 : (OperandSizeOverride ? 2 : 4);
    case 0x6A:                               /* PUSH imm8 */
    case 0x80: case 0x82: case 0x83:         /* arithmetic r/m, imm8 */
    case 0xC0: case 0xC1:                    /* shift r/m, imm8 */
    case 0xCD:                               /* INT imm8 */
    case 0xA8:                               /* TEST AL, imm8 */
        return 1;
    case 0xF6:
        return (ModRmRegField == 0) ? 1 : 0; /* TEST r/m8, imm8 */
    case 0xF7:
        return (ModRmRegField == 0) ?
            (RexW ? 4 : (OperandSizeOverride ? 2 : 4)) : 0;
    case 0xC6:                               /* MOV r/m8, imm8 */
        return 1;
    default:
        return 0;                            /* opcode has no immediate */
    }
}

static BOOLEAN
HookDecOpcodeUsesModRm(
    _In_ UCHAR Opcode
    )
{
    switch (Opcode) {
    case 0x00: case 0x01: case 0x02: case 0x03: case 0x08: case 0x09:
    case 0x0A: case 0x0B: case 0x10: case 0x11: case 0x12: case 0x13:
    case 0x18: case 0x19: case 0x1A: case 0x1B: case 0x20: case 0x21:
    case 0x22: case 0x23: case 0x28: case 0x29: case 0x2A: case 0x2B:
    case 0x30: case 0x31: case 0x32: case 0x33: case 0x38: case 0x39:
    case 0x3A: case 0x3B:
    case 0x62: case 0x63:
    case 0x69: case 0x6B:
    case 0x80: case 0x81: case 0x82: case 0x83:
    case 0x84: case 0x85: case 0x86: case 0x87: case 0x88: case 0x89:
    case 0x8A: case 0x8B: case 0x8C: case 0x8D: case 0x8E: case 0x8F:
    case 0xC0: case 0xC1: case 0xC6: case 0xC7:
    case 0xD0: case 0xD1: case 0xD2: case 0xD3:
    case 0xF6: case 0xF7: case 0xFE: case 0xFF:
        return TRUE;
    default:
        return FALSE;
    }
}

static BOOLEAN
HookDecOpcodeWithoutModRmSupported(
    _In_ UCHAR Opcode
    )
{
    if ((Opcode >= 0x50 && Opcode <= 0x5F) ||
        (Opcode >= 0xB0 && Opcode <= 0xBF)) {
        return TRUE;
    }
    switch (Opcode) {
    case 0x68: case 0x6A:
    case 0x90: case 0x98: case 0x99:
    case 0x9C: case 0x9D:
    case 0xF8: case 0xF9: case 0xFA:
    case 0xFB: case 0xFC: case 0xFD:
        return TRUE;
    default:
        return FALSE;
    }
}

static BOOLEAN
HookDecInstruction(
    _Inout_ HOOK_DECODER* Dec,
    _Out_ HOOK_INSTR_INFO* Info
    )
{
    ULONG startOffset = Dec->Offset;
    UCHAR b;
    UCHAR rex = 0;
    UCHAR rexW = 0;
    UCHAR operandSizeOverride = 0;
    BOOLEAN ripRelative = FALSE;
    ULONG disp32InstrOffset = 0;

    RtlZeroMemory(Info, sizeof(*Info));
    if (!HookDecHasBytes(Dec, 1)) {
        return FALSE;
    }

    /* Legacy prefixes (at most one per group in practice). */
    for (;;) {
        if (!HookDecHasBytes(Dec, 1)) {
            return FALSE;
        }
        b = HookDecPeek(Dec);
        if (b == 0xF0 || b == 0xF2 || b == 0xF3 ||
            b == 0x2E || b == 0x36 || b == 0x3E || b == 0x26 ||
            b == 0x64 || b == 0x65 ||
            b == 0x66 || b == 0x67) {
            if (b == 0x67) {
                return FALSE;   /* address-size override changes RIP rules */
            }
            if (b == 0x66) {
                operandSizeOverride = 1;
            }
            HookDecRead(Dec);
            continue;
        }
        break;
    }

    /* REX prefix (0x40-0x4F). */
    if (!HookDecHasBytes(Dec, 1)) {
        return FALSE;
    }
    b = HookDecPeek(Dec);
    if (b >= 0x40 && b <= 0x4F) {
        rex = HookDecRead(Dec);
        rexW = (UCHAR)((rex >> 3) & 1);
        if (!HookDecHasBytes(Dec, 1)) {
            return FALSE;
        }
        b = HookDecPeek(Dec);
    }

    if (b == 0x0F || b == 0x62 || b == 0xC4 || b == 0xC5) {
        return FALSE;   /* legacy multi-byte, EVEX, and VEX maps unsupported */
    }

    if (b == 0xCD || b == 0xE8 || b == 0xE9 || b == 0xEB ||
        (b >= 0x70 && b <= 0x7F) ||
        (b >= 0xE0 && b <= 0xE3)) {
        return FALSE;   /* relative control flow requires relocation */
    }

    HookDecRead(Dec);                          /* opcode */

    if (HookDecOpcodeUsesModRm(b)) {
        UCHAR modRm;
        UCHAR regField;
        BOOLEAN wasRipRelative = FALSE;
        ULONG disp = 0;
        if (!HookDecHasBytes(Dec, 1)) {
            return FALSE;
        }
        modRm = HookDecRead(Dec);
        regField = (UCHAR)((modRm >> 3) & 7);
        if ((b == 0x8F && regField != 0) ||
            ((b == 0xC6 || b == 0xC7) && regField != 0) ||
            (b == 0xFE && regField > 1) ||
            (b == 0xFF && regField >= 2 && regField <= 5)) {
            return FALSE;
        }
        if (!HookDecConsumeModRmTail(
                Dec, modRm, startOffset,
                &wasRipRelative, &disp)) {
            return FALSE;
        }
        ripRelative = wasRipRelative;
        disp32InstrOffset = disp;

        {
            ULONG imm = HookDecImmediateSize(
                b, rexW, operandSizeOverride, regField);
            if (imm != 0) {
                if (!HookDecHasBytes(Dec, imm)) {
                    return FALSE;
                }
                Dec->Offset += imm;
            }
        }
    } else {
        ULONG imm = HookDecImmediateSize(
            b, rexW, operandSizeOverride, 0);
        if (imm == 0 && !HookDecOpcodeWithoutModRmSupported(b)) {
            return FALSE;
        }
        if (imm != 0) {
            if (!HookDecHasBytes(Dec, imm)) {
                return FALSE;
            }
            Dec->Offset += imm;
        }
    }

    Info->Length = Dec->Offset - startOffset;
    Info->RipRelative = ripRelative;
    Info->Disp32Offset = ripRelative ? disp32InstrOffset : 0;
    return Info->Length != 0;
}

static BOOLEAN
HookTrampolineRel32Fits(
    _In_ LONG64 Displacement
    )
{
    return Displacement >= HOOK_REL32_MIN &&
           Displacement <= HOOK_REL32_MAX;
}

static VOID
HookTrampolineEncodeAbsoluteJmp(
    _Out_writes_bytes_(HOOK_TRAMPOLINE_JMP_SIZE) PUCHAR Out,
    _In_ ULONG64 TargetVa
    )
{
    Out[0] = 0xFF;
    Out[1] = 0x25;
    Out[2] = 0;
    Out[3] = 0;
    Out[4] = 0;
    Out[5] = 0;
    Out[6] = (UCHAR)(TargetVa & 0xFF);
    Out[7] = (UCHAR)((TargetVa >> 8) & 0xFF);
    Out[8] = (UCHAR)((TargetVa >> 16) & 0xFF);
    Out[9] = (UCHAR)((TargetVa >> 24) & 0xFF);
    Out[10] = (UCHAR)((TargetVa >> 32) & 0xFF);
    Out[11] = (UCHAR)((TargetVa >> 40) & 0xFF);
    Out[12] = (UCHAR)((TargetVa >> 48) & 0xFF);
    Out[13] = (UCHAR)((TargetVa >> 56) & 0xFF);
}

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
HookTrampolineBuild(
    _In_ PUCHAR OriginalVa,
    _In_ ULONG MinBytes,
    _Out_ PVOID* TrampolineVirtual,
    _Out_ ULONG* BytesCopied
    )
{
    UCHAR original[HOOK_TRAMPOLINE_MAX_ORIGINAL];
    MM_COPY_ADDRESS source;
    SIZE_T originalBytes = 0;
    PUCHAR buffer;
    PUCHAR out;
    ULONG copied;
    HOOK_DECODER dec;

    *TrampolineVirtual = NULL;
    *BytesCopied = 0;

    if (OriginalVa == NULL || MinBytes == 0 ||
        MinBytes > HOOK_TRAMPOLINE_MAX_ORIGINAL) {
        return STATUS_INVALID_PARAMETER;
    }

    source.VirtualAddress = OriginalVa;
    (VOID)MmCopyMemory(
        original, source, sizeof(original),
        MM_COPY_MEMORY_VIRTUAL, &originalBytes);
    if (originalBytes < MinBytes) {
        return STATUS_PARTIAL_COPY;
    }

    buffer = (PUCHAR)ExAllocatePool2(
        POOL_FLAG_NON_PAGED_EXECUTE | POOL_FLAG_UNINITIALIZED,
        HOOK_TRAMPOLINE_ALLOC, HV_POOL_TAG_HOOK_CODE);
    if (buffer == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    dec.Start = original;
    dec.Offset = 0;
    dec.Length = (ULONG)originalBytes;
    out = buffer;
    copied = 0;

    NT_ASSERT(!HookTrampolineRel32Fits(HOOK_REL32_MIN - 1));
    NT_ASSERT(HookTrampolineRel32Fits(HOOK_REL32_MIN));
    NT_ASSERT(HookTrampolineRel32Fits(HOOK_REL32_MAX));
    NT_ASSERT(!HookTrampolineRel32Fits(HOOK_REL32_MAX + 1));

    while (copied < MinBytes) {
        HOOK_INSTR_INFO info;
        ULONG instrStart = dec.Offset;
        ULONG64 instrVa;
        ULONG64 targetVa;

        if (!HookDecInstruction(&dec, &info)) {
            ExFreePoolWithTag(buffer, HV_POOL_TAG_HOOK_CODE);
            return STATUS_ILLEGAL_INSTRUCTION;
        }
        if (copied + info.Length > HOOK_TRAMPOLINE_MAX_ORIGINAL) {
            ExFreePoolWithTag(buffer, HV_POOL_TAG_HOOK_CODE);
            return STATUS_BUFFER_TOO_SMALL;
        }

        RtlCopyMemory(out, original + instrStart, info.Length);

        if (info.RipRelative) {
            LONG origDisp;
            LONG newDisp32;
            LONG64 newDisp;
            origDisp = (LONG)((ULONG)original[instrStart + info.Disp32Offset] |
                              ((ULONG)original[instrStart + info.Disp32Offset + 1] << 8) |
                              ((ULONG)original[instrStart + info.Disp32Offset + 2] << 16) |
                              ((ULONG)original[instrStart + info.Disp32Offset + 3] << 24));
            instrVa = (ULONG64)OriginalVa + instrStart;
            targetVa = instrVa + info.Length + (ULONG64)(LONG64)origDisp;
            newDisp = (LONG64)targetVa - ((LONG64)((ULONG64)out + info.Length));
            if (!HookTrampolineRel32Fits(newDisp)) {
                ExFreePoolWithTag(buffer, HV_POOL_TAG_HOOK_CODE);
                return STATUS_NOT_SUPPORTED;
            }
            newDisp32 = (LONG)newDisp;
            out[info.Disp32Offset] = (UCHAR)((ULONG)newDisp32 & 0xFF);
            out[info.Disp32Offset + 1] = (UCHAR)(((ULONG)newDisp32 >> 8) & 0xFF);
            out[info.Disp32Offset + 2] = (UCHAR)(((ULONG)newDisp32 >> 16) & 0xFF);
            out[info.Disp32Offset + 3] = (UCHAR)(((ULONG)newDisp32 >> 24) & 0xFF);
        }

        out += info.Length;
        copied += info.Length;
    }

    HookTrampolineEncodeAbsoluteJmp(
        out, (ULONG64)OriginalVa + copied);
    out += HOOK_TRAMPOLINE_JMP_SIZE;

    *TrampolineVirtual = buffer;
    *BytesCopied = copied;
    return STATUS_SUCCESS;
}

_IRQL_requires_(PASSIVE_LEVEL)
VOID
HookTrampolineFree(
    _In_ PVOID TrampolineVirtual
    )
{
    if (TrampolineVirtual != NULL) {
        ExFreePoolWithTag(TrampolineVirtual, HV_POOL_TAG_HOOK_CODE);
    }
}
