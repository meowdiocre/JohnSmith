option casemap:none

EXTERN HookObserveDispatch:PROC

.code

; Stack frame layout (offsets from RBP).
;
; MOVDQU deliberately keeps the dispatcher safe when a hook target does not
; satisfy the function-entry stack-alignment convention.
DISP_HOOKID        EQU -008h
DISP_RETADDR       EQU -010h
DISP_RAX           EQU -018h
DISP_RCX           EQU -020h
DISP_RDX           EQU -028h
DISP_R8            EQU -030h
DISP_R9            EQU -038h
DISP_R10           EQU -040h
DISP_R11           EQU -048h
DISP_TRAMPOLINE    EQU -050h
DISP_RFLAGS        EQU -058h
; -060h: 8 bytes of scratch/padding so the XMM save area below starts on a
; 16-byte boundary relative to RBP.
DISP_XMM0          EQU -070h   ; 16-byte aligned: (0 - 070h) mod 16 = 0
DISP_XMM1          EQU -080h
DISP_XMM2          EQU -090h
DISP_XMM3          EQU -0A0h
DISP_XMM4          EQU -0B0h
DISP_XMM5          EQU -0C0h
DISP_FRAME_SIZE    EQU 0E0h    ; 0C0h locals + 20h outgoing home space

AsmHookDispatcher PROC FRAME

    ; The shadow patch pushed R10 and HookId. Describe those 16 bytes as
    ; pre-entry allocation so virtual unwind reaches the original caller.
    .allocstack 10h
    push    rbp
    .pushreg rbp
    sub     rsp, DISP_FRAME_SIZE
    .allocstack DISP_FRAME_SIZE
    lea     rbp, [rsp+DISP_FRAME_SIZE]
    .setframe rbp, DISP_FRAME_SIZE
    .endprolog

    ; Save the caller's return address, stacked HookId, and every volatile
    ; register the original function may rely on.
    mov     [rbp+DISP_RAX], rax
    pushfq
    pop     rax
    mov     [rbp+DISP_RFLAGS], rax
    mov     rax, [rbp+18h]
    mov     [rbp+DISP_RETADDR], rax
    mov     rax, [rbp+08h]
    mov     [rbp+DISP_HOOKID], rax
    mov     [rbp+DISP_RCX], rcx
    mov     [rbp+DISP_RDX], rdx
    mov     [rbp+DISP_R8],  r8
    mov     [rbp+DISP_R9],  r9
    mov     [rbp+DISP_R10], r10
    mov     [rbp+DISP_R11], r11
    movdqu  [rbp+DISP_XMM0], xmm0
    movdqu  [rbp+DISP_XMM1], xmm1
    movdqu  [rbp+DISP_XMM2], xmm2
    movdqu  [rbp+DISP_XMM3], xmm3
    movdqu  [rbp+DISP_XMM4], xmm4
    movdqu  [rbp+DISP_XMM5], xmm5

    ; HookObserveDispatch(HookId, CallerRetAddr) records the hit, invokes the
    ; observer, and returns the trampoline VA in RAX.
    mov     rcx, [rbp+DISP_HOOKID]
    mov     rdx, [rbp+DISP_RETADDR]
    call    HookObserveDispatch
    test    rax, rax
    jz      DispatchAbort
    mov     [rbp+DISP_TRAMPOLINE], rax

    ; Restore every volatile register the original function expects.
    mov     rax, [rbp+DISP_RAX]
    mov     rcx, [rbp+DISP_RCX]
    mov     rdx, [rbp+DISP_RDX]
    mov     r8,  [rbp+DISP_R8]
    mov     r9,  [rbp+DISP_R9]
    mov     r11, [rbp+DISP_R11]
    mov     [rbp+08h], r11
    movdqu  xmm0, [rbp+DISP_XMM0]
    movdqu  xmm1, [rbp+DISP_XMM1]
    movdqu  xmm2, [rbp+DISP_XMM2]
    movdqu  xmm3, [rbp+DISP_XMM3]
    movdqu  xmm4, [rbp+DISP_XMM4]
    movdqu  xmm5, [rbp+DISP_XMM5]

    ; Restore flags last. MOV/POP/JMP below do not alter them. The indirect
    ; memory jump preserves every guest register, including RAX and R11.
    push    qword ptr [rbp+DISP_RFLAGS]
    popfq
    lea     rsp, [rbp]
    pop     rbp
    pop     r11
    pop     r10
    ; At this point RSP = original_entry_rsp + 10h (the two shadow-patch
    ; pushes have been undone) and [RSP-68h] addresses the trampoline slot
    ; that was at [RBP+DISP_TRAMPOLINE] = [RBP-50h]:
    ;   RBP - 50h = (RSP + 18h - 10h - 8h) - 50h wait, walking it:
    ;   Immediately before pop r10, RSP = RBP + 10h (frame torn down, rbp
    ;   restored, HookId popped). After pop r10 RSP = RBP + 18h.
    ;   [RSP - 68h] = [RBP + 18h - 68h] = [RBP - 50h] = DISP_TRAMPOLINE.
    jmp     qword ptr [rsp-068h]

DispatchAbort:
    ; Publication and retirement keep this unreachable. Preserve the original
    ; machine state if metadata corruption still makes lookup fail.
    mov     rax, [rbp+DISP_RAX]
    mov     rcx, [rbp+DISP_RCX]
    mov     rdx, [rbp+DISP_RDX]
    mov     r8,  [rbp+DISP_R8]
    mov     r9,  [rbp+DISP_R9]
    mov     r11, [rbp+DISP_R11]
    mov     [rbp+08h], r11
    movdqu  xmm0, [rbp+DISP_XMM0]
    movdqu  xmm1, [rbp+DISP_XMM1]
    movdqu  xmm2, [rbp+DISP_XMM2]
    movdqu  xmm3, [rbp+DISP_XMM3]
    movdqu  xmm4, [rbp+DISP_XMM4]
    movdqu  xmm5, [rbp+DISP_XMM5]
    push    qword ptr [rbp+DISP_RFLAGS]
    popfq
    lea     rsp, [rbp]
    pop     rbp
    pop     r11
    pop     r10
    ret

AsmHookDispatcher ENDP

END
