# AMD SVM/NPT map

Authority: [AMD APM Volume 2, 24593 rev.
3.44](https://docs.amd.com/v/u/en-US/24593_3.44_APM_Vol2) and [Volume 3,
24594 rev. 3.37](https://docs.amd.com/v/u/en-US/24594_3.37).

| Code concern | APM location |
| --- | --- |
| SVM/NPT/ASID support | CPUID `Fn8000_000A`; Chapter 15 |
| Enable/disable | `EFER.SVME`, `VM_CR.SVMDIS`, `VM_HSAVE_PA` |
| VMCB layout | Volume 2 Appendix B, Tables B-1/B-2 |
| Exit codes | Appendix C |
| MSRPM | Section 15.11 |
| VMCB clean bits | Section 15.15 |
| ASID/TLB flush | Section 15.16 |
| Event injection | Section 15.20 |
| Nested paging/NPF | Section 15.25 |
| VMRUN/VMMCALL | Volume 3 instruction reference |

Code: `include/amd.h`, `src/amd.c`, `src/amd/`, `asm/amd.asm`.

Keep `C_ASSERT` checks for every assembly-visible VMCB offset. Derive SVM, NPT,
nRIP, ASID count, physical width, and flush behavior at runtime. Clear relevant
VMCB clean bits after software changes cached state. Unexpected NPF exits
fail-stop with `EXITINFO1/EXITINFO2` instead of being hidden as guest
exceptions, and every active CPU is flushed after a live NPT permission/mapping
change. The project 512 GiB identity map is an implementation ceiling.

`CPUID Fn8000_000A.EBX` is an ASID count, including reserved host ASID zero;
valid guest ASIDs are therefore `1..AsidCount-1`. Numeric ASIDs need not be
globally unique across logical processors. JohnSmith keeps one pinned VMCB/NPT
context per CPU, so the existing processor-local assignment and fallback to
ASID 1 are valid; a global ASID allocator would add synchronization without
avoiding any flush.

CPUID executes natively without intercept. AMD InterceptMisc1 bit 18 (IntCpuid)
is cleared; the CPU retires CPUID in guest mode with zero VM-exit overhead.
Since the handler previously applied no feature masks, visibility is unchanged.

I/O intercepts are disabled until JohnSmith owns a real port policy; allocating
an all-zero IOPM only adds state without behavior.

JohnSmith conservatively rejects `CR4.CET=1` on AMD. CET/SSP VMCB state and a
root shadow-stack design are not implemented; do not copy an Intel-only CET
decision into the SVM backend.
