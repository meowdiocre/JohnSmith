option casemap:none

PUBLIC JsCpuidHypercall

.code

JsCpuidHypercall PROC
    push rbx
    mov r10, r8
    mov rbx, rdx
    mov eax, 1
    cpuid
    mov dword ptr [r10+00h], eax
    mov dword ptr [r10+04h], ebx
    mov dword ptr [r10+08h], ecx
    mov dword ptr [r10+0Ch], edx
    pop rbx
    ret
JsCpuidHypercall ENDP

END
