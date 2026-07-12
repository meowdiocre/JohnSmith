# Intel and AMD architecture audit

Audit date: 2026-07-12.

Normative sources:

- Intel 64 and IA-32 SDM, publication 325462 revision 092.
- AMD64 APM Volume 2, publication 24593 revision 3.44.

Research papers and the Microsoft TLFS were not used as architectural authority.

## Corrected findings

### Intel NMI-window VM-entry consistency

**Severity:** critical

The removed NMI-window scaffold enabled primary processor control bit 22 without
enabling NMI exiting and virtual NMIs. Intel SDM revision 092, VM-entry control
checks in Volume 3C, requires NMI-window exiting to be zero when virtual NMIs is
zero, and virtual NMIs to be zero when NMI exiting is zero. The old request path
could therefore make the next VM entry fail.

The scaffold and its unused per-vCPU pending fields were removed. NMI-window
support must not return until all prerequisite controls, NMI-exit handling, event
priority, and queueing are implemented together.

### Intel double-fault classification

**Severity:** high

`#DE` was incorrectly classified as contributory. Intel SDM Volume 3A Table 7-5
classifies divide error as benign. The classifier now treats it as benign.

A pending `#DF` was also treated as shutdown for every second event. Table 7-5
specifies serial handling for `#DF` followed by a benign event and shutdown only
for a following contributory exception or page fault. The fail-stop condition is
now limited to non-benign second exceptions.

### Intel and AMD guest XSETBV

**Severity:** critical

Both exit handlers executed `_xsetbv` directly while running in host/root context.
XCR0 is processor state and is not automatically switched by VMX VM entry/exit or
SVM VMRUN/#VMEXIT. A guest XSETBV therefore changed the host XCR0 and could make
subsequent host code use a state layout inconsistent with Windows.

Both intercepted paths now inject `#GP` without advancing guest RIP. Complete
support requires a per-vCPU guest XCR0, CPUID leaf `0xD` policy consistent with
that value, and assembly save/restore at every host/guest transition.

### AMD VM_CR writes

**Severity:** high

The guest model accepted arbitrary 64-bit values. AMD APM Volume 2 section 15.30.1
requires bits 63:5 to be zero and specifies that LOCK and SVMDIS writes are
ignored while LOCK is set. The handler now rejects reserved-bit writes with
`#GP` and preserves LOCK/SVMDIS while the virtual LOCK bit is set.

This is still a deliberately incomplete nested-SVM model: SVM_KEY-based unlocking
is not virtualized, and SVM remains hidden through CPUID.

### AMD VM_HSAVE_PA writes

**Severity:** high

The guest model accepted unaligned or out-of-range values. AMD APM Volume 2
section 15.30.4 specifies `#GP` when low 12 bits are nonzero or the address is at
or above the implementation's maximum physical address. The handler now enforces
both conditions using CPUID `0x80000008` physical-address width.

## Open findings

### AMD EXITINTINFO collision combining

**Severity:** high

`AmdInjectException` fail-stops whenever EVENTINJ already contains restored
EXITINTINFO. AMD APM Volume 2 sections 15.7.2, 15.7.3, and 15.20 require a VMM
that reflects the intercepted exception to combine it with EXITINTINFO according
to x86 exception rules. Implement the benign/contributory/page-fault table,
`#DF` promotion, and shutdown policy before claiming event robustness on AMD.

### Intel RDTSCP exposes host TSC_AUX

**Severity:** medium

The RDTSCP emulation path returns the AUX value from host `__rdtscp`. If the
RDTSC-exiting control is forced on a processor and this path executes, the guest
sees host `IA32_TSC_AUX`. Add per-vCPU guest TSC_AUX virtualization or reject the
configuration. The normal requested controls do not enable RDTSC exiting.

### Intel event serialisation policy needs hardware tests

**Severity:** medium

The Intel collision path now follows Table 7-5 for classification and `#DF`
promotion, but serial pairs are implemented by replacing the restored event with
the new exception and retrying the original instruction later. Validate nested
faults, software exceptions, NMI interactions, and instruction-length handling
on hardware before treating this as production-ready.

### Host debug-register ownership

**Severity:** high

MOV-DR emulation accesses physical DR0-DR6 in VM-exit context. Audit and test the
assembly boundary to prove host debug state is preserved independently from guest
state on every fast, slow, resume, and VMXOFF path. Do not enable additional debug
intercepts until this ownership is explicit.

### Guest XCR0 virtualization is absent

**Severity:** medium

XSETBV is now safe but unsupported on both backends. Implementing it requires
transition-level host/guest XCR0 switching, not only architectural value
validation in C.

## Comment-style audit

No JSDoc/Doxygen comments (`/**`, `///`, `@param`, `@return`, and related forms)
exist in first-party `src/`, `include/`, or `asm/` files. Matches are under the
vendored `external/KDU` tree and were left unchanged. Rewriting third-party
comments would create an unnecessary vendor diff and violate source ownership.

The obsolete long-form comments and declarations attached to the invalid Intel
window scaffold were removed with that code. Remaining first-party comments use
ordinary C block or line syntax and document architecture invariants, ownership,
or failure policy.
