# Reference catalog

Snapshot verified: **2026-07-19**.

## Architecture

| Source | Revision | Local copy | Used by |
| --- | --- | --- | --- |
| Intel 64 and IA-32 SDM, combined volumes | 092, June 2026 | [325462-092](../static/docs/325462-092-sdm-vol-1-2abcd-3abcd-4.pdf) | VMX, VMCS, EPT, VPID, exceptions, and MSRs |
| AMD64 APM Volume 2: System Programming | 24593 rev. 3.44, March 2026 | [24593-3.44](../static/docs/24593_3.44_APM_Vol2.pdf) | SVM, VMCB, NPT, ASIDs, and event injection |

Official indexes:

- [Intel software developer manuals](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html)
- [AMD APM Volume 2](https://docs.amd.com/v/u/en-US/24593_3.44_APM_Vol2)
- [Microsoft Windows driver DDI reference](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/)

## Implementation guidance

| Source | Revision | Local copy | Used by |
| --- | --- | --- | --- |
| Intel Optimization Reference Manual, Volume 1 | 248966-050, April 2024 | [Intel ORM v050](../static/docs/248966-050-intel-optimization-vol1.pdf) | Cache, TLB, branch, and locality guidance |
| Microsoft x64 ABI | Current online | [x64 calling convention](https://learn.microsoft.com/en-us/cpp/build/x64-calling-convention?view=msvc-170) | Register volatility, stack alignment, and shadow space |

## Local-file integrity

| File | SHA-256 |
| --- | --- |
| `24593_3.44_APM_Vol2.pdf` | `465454E3E7761B126075C789B4BF8B73190F8284637BE604D053252DAB6C11FD` |
| `248966-050-intel-optimization-vol1.pdf` | `4DA42303A8D82A0B4E4FFEBFDB669D6B57D85A038887421339E25CA37B67D61B` |
| `325462-092-sdm-vol-1-2abcd-3abcd-4.pdf` | `16A9336104750613AE2F2BAB6EB7A1B21A7E1EF60CED35E9AB2E0D8C7EFCEC68` |

## Refresh procedure

1. Check the official Intel, AMD, and Microsoft indexes.
2. Record the document ID, revision, release date, and canonical URL.
3. Download the unmodified public original.
4. Verify the cover metadata and SHA-256.
5. Re-audit affected code before updating architectural claims.
