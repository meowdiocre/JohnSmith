# Intel VMX/EPT map

Authority: [Intel 64 and IA-32 SDM version
092](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html),
mainly Volume 3C.

| Code concern | SDM location |
| --- | --- |
| VMX/firmware support | CPUID VMX; `IA32_FEATURE_CONTROL` |
| VMXON/VMCS pages | VMCS region; Section 27.11.5, VMXON Region |
| Control masks/CR rules | Appendix A capability MSRs; CR fixed MSRs |
| VMCS fields | Chapter 27 VMCS tables |
| VM entry/exit | Chapters 29-30 |
| Event reinjection | VM-entry injection and IDT-vectoring fields |
| EPT/VPID | Section 31.3 and `IA32_VMX_EPT_VPID_CAP` |
| INVEPT/INVVPID/VMCALL | VMX instruction reference |

Code: `include/intel.h`, `src/intel.c`, `src/intel/`, `asm/intel.asm`.

Always derive revision ID, allowed controls, CR masks, EPT capabilities,
physical width, and invalidation types at runtime. Treat EPT violation and EPT
misconfiguration separately. After a live EPT permission/mapping change,
invalidate every active CPU before resuming it. The project 512 GiB identity map
is an implementation ceiling. VMCS has an XSS-exiting bitmap, but no
guest-XSS or host-XSS state fields; do not invent such encodings.

Control capability MSRs may require reserved-one settings. Apply the low/high
capability masks and preserve the resulting mandatory bits; do not reject a
control merely because the adjusted value contains bits absent from the
requested functional set. Validate required functional controls and their
architectural dependencies separately.

CR0/CR4 guest-host mask bits set to one are host-owned. For VMX fixed-bit
virtualization, derive those masks as `FIXED0 | ~FIXED1`, adding `CR4.VMXE` to
the CR4 mask. `FIXED0 ^ FIXED1` selects the flexible positions and is the
inverse of the required ownership policy.

When advancing guest RIP after an intercepted instruction, use 64-bit
arithmetic only for an IA-32e guest executing in a 64-bit code segment
(`CS.L=1`). Otherwise, truncate the result to 32 bits so EIP wraps at the 4 GiB
boundary. Intel SDM Section 29.3.1.4 requires guest RIP bits 63:32 to be zero
when IA-32e guest mode is disabled or `CS.L=0`; writing `0x100000000` after an
exit at EIP `0xFFFFFFFE` therefore causes VM-entry failure 33.

An EPT violation reports a disallowed guest-physical access; it is not a guest
page fault. JohnSmith has no instruction emulator or recoverable EPT-policy
callback, so unexpected EPT violations fail-stop with the `EPTV` signature and
retain qualification, guest-physical address, and guest RIP. Never synthesize
`#PF` merely from EPT qualification bits. The Intel per-CPU context also keeps
the latest 64 VM exits for kernel-memory-dump diagnosis. `ExitSequence` is
updated on handler entry, while `CompletedExitSequence` and the paired TSC
values identify whether the corresponding handler reached its return path.

For `EPTV`, bugcheck parameters 2 through 4 are the EPT exit qualification,
guest-physical address, and guest RIP. For `IEVT`, parameter 2 packs the
processor index in the high 32 bits and exit reason in the low 32 bits;
parameter 3 packs the pending event in the high 32 bits and attempted new
event in the low 32 bits; parameter 4 is guest RIP.

If a VM exit occurs during event delivery, preserve the original-event fields
for the next VM entry. A handler must not overwrite a valid pending event with
a newly synthesized exception. The current exception bitmap is zero, so an
event collision is an unsupported invariant violation and fail-stops with the
`IEVT` signature instead of risking an invalid injection sequence.

INIT signals unconditionally cause VM exits while the guest is active, and the
processor performs none of normal INIT state changes before that exit (SDM
Section 28.2). JohnSmith does not support processor reset or hotplug, so it
deliberately drops post-boot INIT and resumes the unchanged active VMCS. Since
the guest is not moved into wait-for-SIPI state, a subsequent SIPI is discarded
by the processor without a VM exit. The raw exit-reason word is retained in the
per-CPU history and VM-entry-failure bit 31 is checked before applying this
policy.

`CR4.CET=1` does not by itself mean supervisor CET is active. JohnSmith permits
that state only when `IA32_S_CET=0`, which preserves user-mode CET without
requiring an alternate root shadow stack. Any nonzero `IA32_S_CET` is rejected
on every logical processor until guest/host CET VMCS state, CET entry/exit
controls, root SSP storage, and `#CP` handling are implemented. While active,
the MSR bitmap permits writes of zero to `IA32_S_CET` but injects `#GP` for a
nonzero write so the supervisor-CET-disabled invariant cannot change after
launch. User-mode CET MSRs are not intercepted.
