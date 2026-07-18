option casemap:none

; Cold hook probe target owned by JohnSmith.
;
; The bytes between the function label and RET must:
;   - be at least HOOK_OBSERVE_THUNK_SIZE (21) bytes long, and
;   - decode cleanly with the whitelist in src/hook_trampoline.c
;     (no 0F/62/C4/C5 multi-byte maps, no REL8/REL32 branches,
;      no CD/E8/E9/EB/E0..E3/70..7F, no 67 address-size override,
;      and no RET inside the copied window - the decoder does not
;      support C3/C2).
;
; 48 89 C8                         mov rax, rcx                  ; 3
; 90 x 18                          nop nop ...                   ; 18
; C3                               ret                           ; at offset 21
;
; Total 21 bytes before RET, exactly HOOK_OBSERVE_THUNK_SIZE.
;
; The PASSIVE_LEVEL hypercall worker invokes this through the explicit
; `hook probe` diagnostic command after installation completes.

PUBLIC JohnSmithHookProbeTarget

_HOOKPROBE SEGMENT ALIGN(4096) READ EXECUTE NOPAGE ALIAS(".hprobe") 'CODE'

JohnSmithHookProbeTarget PROC
    DB      048h, 089h, 0C8h            ; mov rax, rcx - exact 3 bytes
    REPT 18
        nop                             ; 90       - 1 byte * 18 = 18 bytes
    ENDM
    ret                                 ; C3       - at offset 21
JohnSmithHookProbeTarget ENDP

_HOOKPROBE ENDS

END
