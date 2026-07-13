# JohnSmith

Minimal blue-pill Windows x64 research hypervisor with Intel VT-x/EPT and
AMD-V/SVM/NPT backends. It is a foundation for controlled red-team research. With the Goal of Stealth measurement, injection, persistence, spoofing, and detection-evasion.

![JohnSmith banner](static/img/main.png)

> [!WARNING]
> **Experimental Hypervisor:** Development of this hypervisor was LLM-assisted, but all code has been manually checked and validated by a human. If you wish to contribute, please review the manual in the docs first. Ensure that any assumptions made during development are always backed by empirical testing.

## Capabilities

| Area | Implementation |
| --- | --- |
| Lifecycle | Synchronized all-CPU launch, rollback, and teardown |
| Intel | VMX, EPT, VPID, VMCS control validation, and CPUID policy |
| AMD | SVM/NPT and masked CPUID policy |
| Memory | 512 GiB identity-SLAT ceiling, partially tested runtime 4 KiB access changes |
| State | CR0/CR3/CR4, debug state, PAT/EFER, and MSR bitmaps modeled in code |
| Diagnostics | Fail-stop bugchecks and Debug-only VM-exit history |
| Measurement | Cross-core software-clock VM-exit benchmark |

Intel and AMD CPUID exits are emulated and vendor-virtualization features are
hidden.

## Documentation

| Document | Purpose |
| :--- | :--- |
| [Implementation Status](docs/implementation-status.md) | Audited code-to-claim matrix and known blockers |
| [Documentation Index](DOCUMENTATION.md) | Scope, policy, and repository map |
| [Intel VMX/EPT](docs/architecture/intel-vmx.md) | VMCS, exits, EPT, VPID, CET |
| [AMD SVM/NPT](docs/architecture/amd-svm.md) | VMCB, exits, NPT, ASIDs |
| [Build and Test](docs/build-and-test.md) | Reproducible build/load workflow instructions |
| [References](docs/references.md) | Manuals, papers, revisions, and hashes |


## License

[MIT](LICENSE)
