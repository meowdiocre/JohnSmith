option casemap:none

.code

; RCX = shared software counter, RDX = output samples, R8D = sample count.
; R9-R11 are volatile under the Windows x64 ABI.  Each probe brackets exactly
; one instruction with loads from the same counter.

MeasureSerialize PROC
MeasureSerializeLoop:
    mov r9, qword ptr [rcx]
    serialize
    mov r10, qword ptr [rcx]
    sub r10, r9
    mov qword ptr [rdx], r10
    add rdx, 8
    sub r8d, 1
    jnz MeasureSerializeLoop
    ret
MeasureSerialize ENDP

MeasureCpuidLeaf0 PROC
    push rbx
    mov r11, rcx
    mov r10, rdx
MeasureCpuidLeaf0Loop:
    mov r9, qword ptr [r11]
    xor eax, eax
    xor ecx, ecx
    cpuid
    mov rax, qword ptr [r11]
    sub rax, r9
    mov qword ptr [r10], rax
    add r10, 8
    sub r8d, 1
    jnz MeasureCpuidLeaf0Loop
    pop rbx
    ret
MeasureCpuidLeaf0 ENDP

MeasureCpuidLeaf16 PROC
    push rbx
    mov r11, rcx
    mov r10, rdx
MeasureCpuidLeaf16Loop:
    mov r9, qword ptr [r11]
    mov eax, 16h
    xor ecx, ecx
    cpuid
    mov rax, qword ptr [r11]
    sub rax, r9
    mov qword ptr [r10], rax
    add r10, 8
    sub r8d, 1
    jnz MeasureCpuidLeaf16Loop
    pop rbx
    ret
MeasureCpuidLeaf16 ENDP

; The benchmark-enabled JohnSmith build recognizes this signature and performs
; a minimal RIP-advance/resume path.  This routine remains a true leaf function
; so Windows can unwind a production-build #UD to the harness SEH filter.
MeasureVmcall PROC
    mov r11, rcx
    mov r10, rdx
    mov r9d, r8d
MeasureVmcallLoop:
    mov rax, qword ptr [r11]
    mov qword ptr [r10], rax
    mov rax, 04A534D5642454E43h
    mov rcx, 0484D41524B464C52h
    mov rdx, 0564D43414C4C3031h
    mov r8,  0B16B00B5DEADC0DEh
    vmcall
    mov rax, qword ptr [r11]
    sub rax, qword ptr [r10]
    mov qword ptr [r10], rax
    add r10, 8
    sub r9d, 1
    jnz MeasureVmcallLoop
    ret
MeasureVmcall ENDP

END
