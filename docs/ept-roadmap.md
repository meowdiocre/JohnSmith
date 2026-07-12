# EPT and runtime-control roadmap

Status: implementation plan. Landed pieces are marked with **[implemented]**;
unmarked bullets are still pending.

Normative references:

- Intel 64 and IA-32 SDM rev. 092: [`static/docs/325462-092-sdm-vol-1-2abcd-3abcd-4.pdf`](../static/docs/325462-092-sdm-vol-1-2abcd-3abcd-4.pdf), especially Vol. 3A exception delivery and Vol. 3C VM exits, VM-entry event injection, EPT, INVEPT, and VMCS controls.
- Existing project interpretation: [`architecture/intel-vmx.md`](architecture/intel-vmx.md).
- AMD64 APM 3.44: [`static/docs/24593_3.44_APM_Vol2.pdf`](../static/docs/24593_3.44_APM_Vol2.pdf) and [`architecture/amd-svm.md`](architecture/amd-svm.md).

## Findings that change the proposed design

1. **An EPT violation is not a guest page fault.** The current architecture document explicitly records this. Blindly converting every EPT violation to `#PF` invents guest first-level paging semantics and can corrupt `CR2`. Only a deliberate EPT policy fault (for example, a hook's denied data/execute access) may be translated into a synthetic guest exception, and its `#PF` error code must be derived from the intended guest-visible policy plus the EPT exit qualification—not copied mechanically.
2. **Guest CR2 is not a VMCS field.** `__writecr2()` in VMX root changes host CR2. The VMCS does not automatically restore CR2 on VM entry/exit. Correct virtualization therefore needs per-vCPU guest/host CR2 ownership and assembly save/restore, or a design that does not synthesize `#PF`. Writing host CR2 immediately before `VMRESUME` is not sufficient or safe.
3. **Execute-only EPT is capability-dependent.** `IntelSetOwnedPageAccess` already checks `IA32_VMX_EPT_VPID_CAP[0]` for execute-only support. A write-without-read EPT entry is unsupported by the current API and is not needed for the proposed hooks.
4. **One EPT cannot map the same GPA to two HPAs simultaneously.** Dual-view hooks require two EPT roots (or VMFUNC/EPTP switching), not two PTEs in one tree. The primary tree should expose the original page as RW/NX; the execution tree should expose the shadow page as X-only.
5. **The existing runtime split and permission operations are substantially implemented.** `IntelSetOwnedPageAccess` splits a 2 MiB PDE, preserves R/W/X and memory type bits, updates one PTE, and calls the generation/IPI/INVEPT path while running. `IntelQueryOwnedPageAccess` handles both large and split mappings. Work should refactor and test these rather than duplicate them.
6. **EPT misconfiguration should remain fail-stop unless recovery is rigorously defined.** Injecting `#MC` does not repair malformed EPT state. Setting guest activity to shutdown silently kills the guest/OS. Keep diagnostic fail-stop for internal corruption; an optional controlled shutdown policy can be added separately.
7. **Unknown VM exits cannot safely become `#GP` generically.** Exit-specific retry/RIP/event semantics differ. The current architecture document deliberately rejects this conversion. Keep fail-stop until each exit reason has a specified handler.
8. **External-interrupt deferral is currently unnecessary.** External-interrupt exiting and acknowledge-on-exit are not enabled. If added, the design needs a queue/bitmap, not one vector, because multiple interrupts may arrive before the window opens; APIC priority and acknowledgement semantics also matter.
9. **XSS is not XCR0.** `IA32_XSS` and `XCR0` select different state components. `VMCS_XSS_EXITING_BITMAP` controls which `IA32_XSS` bits cause `XSAVES/XRSTORS` exits; copying XCR0 into it is incorrect. XSETBV should validate/virtualize XCR0 and preserve host XCR0 around exits if host and guest values may differ.
10. **AMD CPUID currently does not exit.** `amd_vmcb.c` deliberately leaves CPUID interception clear, so adding only a handler is dead code. CPUID interception must first be enabled and documented, with the performance cost accepted.
11. **Private host page tables cannot be a one-time shallow PML4 copy.** Kernel mappings can change after launch. Copying only PML4 entries shares lower levels (not a deep copy), while a true deep copy becomes stale. Self-map discovery is Windows-version-sensitive. This phase requires an explicit mapping-lifetime strategy and must precede claims of host isolation.
12. **Physical-memory IOCTLs are privileged host physical operations, not automatically GPA operations.** In the current identity EPT they coincide, but hooks/remaps break that equivalence. APIs must distinguish GPA, current HPA, and guest virtual address.

## Stage 0 — contracts, capability gates, and tests

- Add explicit types for page-aligned GPA, HPA, EPT permissions, EPT root/view, and hook ID.
- Extend the backend interface only for operations shared by Intel and AMD; keep EPT-specific root/view operations in Intel code.
- Define lock order: hook registry lock → EPT-root lock; never allocate or rendezvous while holding a spin lock or at VM-exit IRQL.
- Define lifetime rules so no PT or shadow page is freed before all active CPUs have invalidated the old translation.
- Add checked arithmetic for GPA/VA + size, page crossing, canonical VAs, map limit, physical-width limits, and user-buffer lengths.
- Add unit-testable helpers for EPT qualification decoding, synthetic PFEC construction, DR qualification decoding, event priority, and guest page-table walking.
- Update `docs/architecture/intel-vmx.md` whenever policy changes; retain exact SDM revision/section citations in code comments.

## Stage 1 — complete and harden the existing EPT primitives

Files: `src/intel/intel_slat.c`, `src/intel/intel_internal.h`, `include/hv.h`, `src/introspection.c`.

1. Refactor the inline 2 MiB split in `IntelSetOwnedPageAccess` into `IntelSplit2MbLocked`. **[implemented]**
   - Allocate the PT and split descriptor before publication.
   - Clone base PFN + 512 page offsets.
   - Preserve PDE R/W/X and memory type; explicitly audit ignored/PAT/suppress-VE/user-execute bits against supported capabilities.
   - Publish the descriptor before atomically publishing the non-leaf PDE.
   - Preserve non-leaf permissions according to project policy.
2. Change address validation to accept arbitrary byte GPA where appropriate, internally selecting the containing 4 KiB page. Keep page-alignment requirements only on APIs that operate on whole pages.
3. Keep `IntelQueryOwnedPageAccess`; add tests for 2 MiB mappings, boot-time mixed splits, runtime splits, boundary addresses, and out-of-range addresses.
4. Add `IntelSetOwnedPageMapping(State, Root, Gpa, Hpa, Access, Previous)` so PFN and permission updates are one atomic operation from the caller's perspective. **[implemented]**
5. Retain generation-based cross-core invalidation. Do not expose `IntelInvalidateRunningSlat` directly; mutation helpers publish state, release the EPT lock, then rendezvous. **[implemented]**
6. Add merge support only when metadata proves all 512 PTEs are contiguous and identical in relevant attributes and no hook/reference remains. Publish the large PDE, invalidate all CPUs, then retire/free the old PT. Never free before invalidation completion. **[implemented for `PERMISSION` splits]**
7. Track why a region is split (mixed memory map, runtime permission, hook). Mixed RAM/MMIO regions must never merge into a single memory type. **[implemented for `MIXED_MEMORY_MAP` and `PERMISSION`; `HOOK` reserved for Stage 4]**

Acceptance: existing introspection test still passes; split preserves WB/UC classification; X-only is accepted only when capability bit 0 is present; live mutation completes on every active CPU.

## Stage 2 — classify EPT exits instead of globally injecting `#PF`

Files: `src/intel/intel_exit.c`, `src/intel/intel_internal.h`, `include/intel.h`, `asm/intel.asm` only if guest CR2 virtualization is selected.

1. Decode and log exit qualification, GPA, and GLA-valid/translation flags. Do not consume GLA unless qualification says it is valid. **[implemented via `IntelDecodeEptViolation`]**
2. Look up the GPA in an immutable/read-safe hook-policy table. **[implemented via `IntelHookLookup`; table population is Stage 4]**
3. If the exit matches a known hook transition, switch EPT view as described in Stage 4 and retry without advancing RIP.
4. If policy intentionally presents a guest page fault:
   - require a valid GLA;
   - construct PFEC (`P`, `W/R`, `U/S`, `I/D`, and only architecturally applicable bits) from guest-visible policy and access type;
   - virtualize guest CR2 safely in assembly/per-vCPU state; **[implemented: asm slow path snapshots CR2 into `INTEL_CPU_CONTEXT::GuestCr2` on exit and restores it before VMRESUME/VMXOFF; C helpers `IntelGetGuestCr2`/`IntelSetGuestCr2`; VMCS setup seeds initial CR2; `VMX_ENTRY_INJECT_PF` and `X86_PFEC_*` constants ready for the injection path]**
   - inject vector 14 with deliver-error-code set;
   - do not advance RIP.
5. Unknown EPT violations remain diagnostic fail-stop initially. This is safer than fabricating a guest fault. **[implemented via `IntelFailEptViolation` with decoded qualification packed into bugcheck parameter 4]**
6. EPT misconfiguration logs GPA, qualification/history, EPTP, and relevant walk entries, then fail-stops. Add a build-time experimental shutdown policy only if required. **[implemented via `IntelFailEptMisconfiguration`; dedicated `INTEL_BUGCHECK_EPT_MISCONFIG` signature]**

Acceptance: instruction and data accesses are distinguished; invalid GLA is never written to guest CR2; no host CR2 leakage; retry preserves RIP.

## Stage 3 — event and exit robustness, one reason at a time

Files: `src/intel/intel_exit.c`, `src/intel/intel_vmcs.c`, `src/intel/intel_internal.h`.

- Replace collision bugchecks only after implementing a table-driven event classifier from SDM event-priority/double-fault rules. Preserve IDT-vectoring error code and instruction length. Test contributory+contributory and PF+contributory → `#DF`; do not reduce all other cases to an undocumented drop. **[partially implemented: Intel classifies Table 7-5 benign, contributory, and page-fault events and promotes the three architectural double-fault pairs. Shutdown remains fail-stop because guest shutdown emulation is absent.]**
- Interrupt-window and NMI-window exiting: enable controls only with complete producers and handlers. NMI-window exiting additionally requires NMI exiting and virtual NMIs under the VM-entry consistency checks. **[not implemented: an invalid scaffold that enabled NMI-window exiting without those prerequisite controls was removed during the manual audit.]**
- Keep unknown-exit fail-stop. Add explicit handlers for each newly enabled control before enabling it.
- MOV-DR: enable/verify the corresponding primary control, decode DR number/direction/GPR, enforce CPL and `CR4.DE`, handle DR4/5 aliases, GD behavior, reserved DR6/DR7 bits, and advance only on success. Preserve host debug-register state across VM exits rather than treating root DR0–DR6 as guest-owned scratch.
- RDTSC/RDTSCP: first inspect adjusted primary controls. If RDTSC exiting is forced, emulate architectural width and TSC offset. For RDTSCP, return guest-visible `IA32_TSC_AUX`, not an unvirtualized host value.
- TSC synchronization: prefer hardware TSC scaling/offset semantics and measure at the actual launch boundary. A single serial IPI callback is not simultaneous, so define the required skew and test it. Do not claim exact synchronization.
- XSETBV: preserve host XCR0 if guest XCR0 differs; do not derive XSS-exiting bitmap from XCR0. Add IA32_XSS virtualization only together with XSAVES policy.

## Stage 4 — dual-EPT code hooks

Files: `src/intel/intel_hook.c`, `src/intel/intel_internal.h`, `src/intel/intel_slat.c`, `src/intel/intel_exit.c`, `src/intel.c`.

1. Introduce an EPT-root object with its own PML4/PDPT/PD/PT metadata and reference counts. Do not reuse one global `SplitList` for multiple roots. **[implemented: `INTEL_EPT_ROOT` owns PML4/PDPT/PDs/`SplitList`/`SlatLock`/`EptPointer`. `INTEL_BACKEND_CONTEXT` embeds `PrimaryRoot` inline and has a `HookRoot*` slot for the secondary root. `IntelBuildIdentityRoot`/`IntelFreeRoot` operate on a single root; `IntelBuildEpt`/`IntelFreeEpt` remain the backend-level entry points.]**
2. Build the secondary root before launch or at PASSIVE_LEVEL. Share immutable tables with copy-on-write, or deep-copy all paging structures; document the chosen ownership model. **[implemented via deep-copy: `IntelHookEnsureSecondaryRoot` allocates a fresh `INTEL_EPT_ROOT` and calls `IntelBuildIdentityRoot(HV_PAGE_ACCESS_READ | HV_PAGE_ACCESS_WRITE)`. Every RAM leaf in the secondary root is therefore R+W (no execute); every non-RAM leaf keeps the same permission mask. This is the mechanism that forces an execute exit whenever control flow leaves a hooked page.]** “Deep-copy EPT” means EPT paging structures, not guest data pages.
3. Allocate a cached contiguous 4 KiB shadow page, copy the original HPA via a supported physical mapping API, validate patch offset/length, and apply patch bytes. Keep an immutable copy or deterministic reconstruction for removal. **[implemented via `IntelHookBuildShadow`: allocates a WB-cached contiguous page under 4 GiB is not required; the shadow is mapped by physical address, so any HPA is fine. Copies the original page through `MmMapIoSpaceEx` (`PAGE_READWRITE`, PASSIVE_LEVEL) and then applies the patch bytes at `PatchOffset`. Original PTE snapshots are retained in the policy slot for deterministic remove.]**
4. Primary root: original HPA, R/W, no execute. Secondary root: shadow HPA, execute-only. Capability-gate execute-only. **[implemented: install rewrites the primary 4 KiB PTE from RWX to R+W (execute stripped, memory type preserved), and rewrites the secondary PTE to X-only pointing at the shadow HPA. `IntelHookInstall` refuses to run when `IA32_VMX_EPT_VPID_CAP[0]` (execute-only pages) is clear.]**
5. VM-exit transition:
   - execute violation on a hooked GPA in primary → write secondary EPTP and retry;
   - data violation while in secondary → restore primary EPTP and retry;
   - execution that leaves the hooked page must be forced to exit. Therefore secondary mappings for non-hook pages need a deliberate no-execute policy or another monitored-transition mechanism. Merely waiting for an “execute violation at non-hooked GPA” will not work if those entries remain executable.
   **[implemented: the EPT-violation handler consults `IntelHookLookup` and calls `IntelSwitchActiveEptRoot` for a matching execute-on-primary or data-on-secondary transition, then returns `INTEL_VMEXIT_RESUME` without advancing RIP. A separate clause handles an execute violation on the secondary root outside any hooked page by swapping back to the primary root; the secondary root's per-page R+W-only leaves guarantee that exit fires. Any policy match that reaches an unswitchable state bugchecks with `INTEL_BUGCHECK_EPT_VIOLATION` carrying `(Cookie, Kind)` in parameter 4 so bring-up can diagnose it.]**
6. Track active view per vCPU and flush the correct EPTP. Any shared-table update invalidates every affected EPT context. **[implemented: per-vCPU `INTEL_CPU_CONTEXT::EptPointer` records the currently loaded root; `IntelSwitchActiveEptRoot` writes VMCS_EPT_POINTER, resets the vCPU's cached `SlatGeneration` to 0 to force mismatch, then invokes `IntelFlushEptIfNeeded` for a single-context INVEPT (falls back to all-context when capability bit 25 is clear).]**
7. Hook removal is transactional: mark retiring, switch all CPUs away from the secondary view, rendezvous/invalidate, restore entries, then free shadow/root/PT resources. Merge only after split reasons and references reach zero. **[implemented: `IntelHookRemove` clears the GPA slot first (release store) so no new exit dispatches to the hook, rendezvous-invalidates every vCPU, then reacquires both root locks, restores the saved PTE snapshots verbatim, and rendezvous-invalidates a second time before freeing the shadow page. The merge helper still refuses to collapse an `INTEL_SPLIT_REASON_HOOK` split even after remove restores permissions; that keeps remove→reinstall cheap at the cost of holding one 4 KiB PT per formerly-hooked 2 MiB region until backend teardown. A future refactor with per-split hook refcounts can retag the split back to `PERMISSION` when its last hook departs.]**

Acceptance: reads return original bytes, fetch executes patched bytes, crossing to another page restores the primary view, removal cannot race an executing CPU, and memory type remains unchanged.

## Stage 5 — AMD CPUID policy

Files: `src/amd/amd_vmcb.c`, `src/amd/amd_exit.c`, `src/amd/amd_internal.h`, docs.

- Decide whether hiding SVM/hypervisor presence is a project requirement worth intercepting every CPUID.
- If yes, enable CPUID interception first and add `AMD_EXIT_CPUID` dispatch using native CPUID plus masks: leaf 1 ECX[31], `0x80000001` ECX[2], and a documented policy for `0x8000000A` consistent with the maximum extended leaf.
- Advance via `NextRip`, clear clean state as needed, and test topology/OSXSAVE leaves. Update the AMD architecture doc, which currently promises native CPUID.

## Stage 6 — device/API surface

Files: `src/driver.c`, `src/device.c`, `include/device.h`, `include/johnsmith_ioctl.h`, project files, security docs.

1. Create `\Device\JohnSmith` and `\DosDevices\JohnSmith`; install create/close/device-control dispatch and delete links/device on every unload/failure path. **[implemented]**
2. Use versioned, fixed-width request headers and `METHOD_BUFFERED` initially. Set restrictive device ACLs; require callers with administrative/debug privilege. Never trust embedded sizes or pointers. **[implemented: `JOHNSMITH_REQUEST_HEADER` with size/version/reserved validation, SYSTEM+Administrators-only SDDL, `IoCreateDeviceSecure`]**
3. Separate commands and terminology:
   - read/write host physical address;
   - resolve/read/write guest physical address through the active EPT view;
   - translate/read/write guest VA using a supplied guest CR3;
   - add/remove/query hook by opaque ID. **[implemented: `IOCTL_JOHNSMITH_HOOK_INSTALL` / `IOCTL_JOHNSMITH_HOOK_REMOVE` / `IOCTL_JOHNSMITH_HOOK_QUERY` route through new backend ops `HookInstall` / `HookRemove` / `HookQuery` (Intel implements; AMD returns `STATUS_NOT_SUPPORTED`). The install request carries an in-band patch buffer capped at `JOHNSMITH_HOOK_MAX_PATCH = 128` bytes; larger patches will need a separate shared-buffer command.]**
4. Implement a guest page-table walker for 4/5-level paging as supported, large pages, canonicality, present/write/NX checks, PCID-masked CR3, physical-width validation, and page-spanning buffers. Revalidate mappings per page.
5. Define write coherency and executable-code synchronization. Arbitrary MMIO physical access is dangerous; deny it by default or require an explicit flag and range policy.
6. Serialize IOCTLs with start/stop and hook retirement. Return exact bytes completed and stable NTSTATUS values.
7. Add VMCALL commands only after IOCTL behavior is tested. Authenticate CPL0 plus a per-boot cookie/capability; validate guest pointers through guest translation; bound operation size; never block or allocate in the VM-exit hot path. Prefer a preallocated shared command page/ring for larger transfers.

## Stage 7 — console

Files: new `tools/johnsmithctl/`, solution/project updates.

- Open `\\.\\JohnSmith`, negotiate API version, and expose `status`, `hooks`, `dump-pa`, `dump-gpa`, `dump-va`, and `translate`.
- Parse 64-bit addresses/sizes strictly, stream large dumps, show partial-transfer errors, and distinguish GPA from HPA in all output.
- Add tests for malformed IOCTL lengths and non-admin access.

## Stage 8 — host page-table isolation research gate

Do not place this on the critical path for EPT hooks.

- Inventory every address root mode touches: VM-exit code, stacks, context, EPT, bitmaps, kernel APIs, IDT/GDT/TSS, unwind/bugcheck/logging data, per-CPU data, and executable pool policy.
- Choose either (a) a minimal static root address space with no calls into arbitrary Windows code, or (b) shared kernel mappings with a clearly stated weaker isolation claim.
- Account for post-launch allocations, dynamic kernel mapping changes, large pages, KVA shadow, self-map/randomization, PCID, global mappings, and TLB shootdowns.
- Allocate and populate root tables before VMCS setup; store/free per-vCPU host CR3; restore ordinary kernel CR3 after VMXOFF. Validate on every supported Windows build.
- Until this is proven, retain current `VMCS_HOST_CR3 = __readcr3()` and make no “private host address space” claim.

## Recommended implementation order

1. Stage 0 contracts/tests.
2. Stage 1 EPT refactor, remap, merge, and invalidation tests.
3. Stage 2 classified EPT exits (without generic `#PF`).
4. Stage 4 dual-root hook prototype and safe removal.
5. Stage 6 IOCTL/status/query surface, then Stage 7 console.
6. Stage 3 robustness handlers individually, enabling controls only with handlers.
7. Stage 5 AMD CPUID only if masking is required.
8. Stage 8 as a separate security project.

## Hardware verification gates

For every stage, build Debug/Release/Benchmark and run WDK analysis. Runtime evidence must include CPU model/microcode, Windows build, VMX capability MSRs, artifact SHA-256, and debugger logs. Required bare-metal tests include all-core launch/stop, repeated split/remap/merge, concurrent access during hook install/remove, instruction/data behavior, INVEPT completion on every CPU, sleep/hotplug policy, and failure injection for every allocation/publication boundary.
