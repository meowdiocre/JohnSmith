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

`VMCS_GUEST_INTERRUPTIBILITY` and `VMCS_GUEST_PENDING_DEBUG` are initialized
to zero once, after `VMCLEAR` and before the first `VMLAUNCH`; normal
`VMRESUME` paths do not rewrite them. Successful instruction emulation clears
the saved STI/MOV-SS shadow because the emulated instruction consumes that
one-instruction blocking window. It also preserves pending debug causes and
sets pending-debug `BS` when `RFLAGS.TF=1` and `IA32_DEBUGCTL.BTF=0`. Current
emulated instructions have no guest data-memory operand, so comparing the new
RIP against DR0-DR3 would incorrectly conflate execution and data breakpoints.
The VM-exit save-debug-controls and VM-entry load-debug-controls controls keep
guest DR7 and IA32_DEBUGCTL in the VMCS. VM exit sets root DR7 to `0x400` and
clears IA32_DEBUGCTL, so root-mode register reads are not guest-state reads.

VM exit also sets the GDTR and IDTR limits to `0xFFFF` (Section 30.5.2).
Before VMXOFF, the stop path captures the current guest descriptor-table bases
and limits; after VMXOFF, it restores them with LGDT and LIDT. Restoring only
the bases, or using launch-time snapshots, leaves Windows with stale state.

CPUID exits unconditionally in VMX non-root operation (Section 28.1.2).
JohnSmith executes CPUID on the current logical processor for every exit because
topology, APIC ID, OSXSAVE, XCR0, and XSS-dependent results are not globally
cacheable. Returned features are masked to match enabled VMX controls and the
no-nested-VMX policy. Caching can shorten a handler but cannot remove the VM
exit or its elapsed time. RDTSC/RDTSCP execute natively: RDTSC exiting and TSC
offsetting remain disabled, so JohnSmith does not claim timing invisibility.

VPIDs are processor-local translation tags; one system-wide VPID is not
required. JohnSmith retains one nonzero VPID per pinned CPU context. Native
MOV-to-CR3 normally does not exit. If hardware forces CR3-load exiting, the
software-emulated CR3 write performs single-context INVVPID before resume
because VMWRITE alone does not reproduce MOV-to-CR3 invalidation semantics.

The MSR bitmap intercepts reads and writes of `IA32_FEATURE_CONTROL` and the
VMX capability range. Reads expose a locked feature-control value with both
VMX-enable bits clear; VMX capability reads and all intercepted unsupported
MSRs receive `#GP`. This keeps MSR behavior coherent with cleared CPUID.VMX.
The VM-exit/entry MSR-list counts remain zero: PAT and EFER already use their
dedicated controls, and JohnSmith does not maintain a separate root MSR
environment.

An EPT violation reports a disallowed guest-physical access; it is not a guest
page fault. JohnSmith has no instruction emulator or recoverable EPT-policy
callback, so unexpected EPT violations fail-stop with the `EPTV` signature and
retain qualification, guest-physical address, and guest RIP. Never synthesize
`#PF` merely from EPT qualification bits. Diagnostic builds keep the latest 64
VM exits for kernel-memory-dump diagnosis. `ExitSequence` is updated on handler
entry, while `CompletedExitSequence` and the paired TSC values identify whether
the corresponding handler reached its return path. Release builds compile this
per-exit history out of the hot path.

For `EPTV`, bugcheck parameters 2 through 4 are the EPT exit qualification,
guest-physical address, and guest RIP. For `IEVT`, parameter 2 packs the
processor index in the high 32 bits and exit reason in the low 32 bits;
parameter 3 packs the pending event in the high 32 bits and attempted new
event in the low 32 bits; parameter 4 is guest RIP.

If a VM exit occurs during event delivery, preserve the original-event fields
for the next VM entry. A handler must not overwrite a valid pending event with
a newly synthesized exception. Invalid-op synthesis is deferred when a pending
event already exists; other collisions fail-stop with the `IEVT` signature
instead of risking an invalid injection sequence.

Unknown or unclassified VM-exit reasons remain fail-stop. Exit qualification,
instruction length, and retry semantics are reason-specific; a generic
`#GP`-and-resume path can loop or corrupt guest state.

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
