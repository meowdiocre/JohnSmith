# AMD SVM/NPT architecture

Implementation map for `src/amd.c`, `src/amd/`, `include/amd.h`, and
`asm/amd.asm`.

Normative source: [AMD64 APM Volume 2, publication 24593 revision 3.44](../../static/docs/24593_3.44_APM_Vol2.pdf).

## Platform gates

The backend requires SVM, an enabled `VM_CR`, a nonzero SVM revision, at least
one guest ASID, nested paging, and NRIPS. Assembly-visible VMCB fields have
compile-time offset assertions.

## VMCB and intercept policy

Each processor owns guest and host VMCBs, host-save memory, an MSR permission
map, a host stack, guest ASID, SLAT generation, and captured teardown state.

The VMCB enables nested paging and intercepts CPUID, selected MSRs, INVLPGA,
SVM instructions, and XSETBV. CPUID emulation hides hypervisor, VMX, SVM, and
SVM-capability exposure while preserving native topology and OS-dependent
results.

MSR handling virtualizes EFER, `VM_CR`, and `VM_HSAVE_PA`. EFER writes are
gated by their CPUID feature bits. `VM_CR` writes enforce reserved, lock, and
disable semantics; `VM_HSAVE_PA` writes enforce alignment and physical-width
constraints. Intercepted SVM instructions and INVLPGA inject `#UD`; XSETBV
injects `#GP`.

## Entry and exit pipeline

`AmdAsmLaunch` saves host state, establishes the host stack, loads guest state,
enables GIF, and executes VMRUN. VM exit saves the guest VMCB, restores the
host VMCB, preserves GPRs and ABI-volatile XMM registers, and calls the C
dispatcher.

The dispatcher consumes pending NPT invalidation, restores `EXITINTINFO`,
handles the exact exit code, advances to `nRIP` for completed emulation, and
updates VMCB clean bits. Unexpected exits, nested page faults, invalidation
failures, and event collisions use separate fail-stop signatures.

## NPT and ASIDs

The identity NPT maps up to 512 GiB. Cache flags are derived from the active
PAT. Uniform ranges use 2 MiB entries; mixed RAM and non-RAM ranges use 4 KiB
page tables.

Runtime permission changes increment a shared generation and rendezvous active
processors through a signature-gated CPL0 VMMCALL. Each VMCB consumes the
generation with `TLB_CONTROL=3` when flush-by-ASID is available and
`TLB_CONTROL=1` otherwise.

Guest ASIDs use the range `1..AsidCount-1`. Each VMCB remains pinned to its
logical processor.

## Teardown

VMRUN and VMEXIT automatic state is supplemented by VMSAVE and VMLOAD. The
stop path reconstructs Windows control and debug state from the guest VMCB,
restores PAT, EFER, and `VM_HSAVE_PA`, enables GIF, and returns to the captured
RIP and RSP.
