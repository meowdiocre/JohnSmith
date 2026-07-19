#include <assert.h>

#include "../src/intel/intel_rendezvous_policy.h"

int
main(void)
{
    unsigned budget;
    unsigned index;

    assert(INTEL_RENDEZVOUS_ICR_LOW == 0x000C4400u);
    assert(IntelRendezvousClassifyPolicy(
        INTEL_POLICY_EXIT_CPUID, 0, 0) == INTEL_POLICY_MANDATORY);
    assert(IntelRendezvousClassifyPolicy(
        INTEL_POLICY_EXIT_EPT_VIOLATION, 0, 0) ==
        INTEL_POLICY_MANDATORY);
    assert(IntelRendezvousClassifyPolicy(
        INTEL_POLICY_EXIT_RDMSR, INTEL_POLICY_IA32_TSC, 0) ==
        INTEL_POLICY_MANDATORY);
    assert(IntelRendezvousClassifyPolicy(
        INTEL_POLICY_EXIT_CR_ACCESS, 0, 1) == INTEL_POLICY_CONDITIONAL);
    assert(IntelRendezvousClassifyPolicy(
        INTEL_POLICY_EXIT_CR_ACCESS, 0, 0) == INTEL_POLICY_NONE);
    assert(IntelRendezvousClassifyPolicy(
        INTEL_POLICY_EXIT_EXTERNAL_INTERRUPT, 0, 8) ==
        INTEL_POLICY_EXCLUDED);
    assert(IntelRendezvousClassifyPolicy(
        INTEL_POLICY_EXIT_MTF, 0, 8) == INTEL_POLICY_EXCLUDED);
    assert(IntelRendezvousClassifyPolicy(
        INTEL_POLICY_EXIT_PREEMPTION_TIMER, 0, 8) ==
        INTEL_POLICY_EXCLUDED);

    budget = INTEL_RENDEZVOUS_HOOK_BUDGET;
    for (index = 0; index < INTEL_RENDEZVOUS_HOOK_BUDGET; ++index) {
        budget = IntelRendezvousConsumeBudget(
            INTEL_POLICY_EXIT_CPUID, budget);
    }
    assert(budget == 0);
    assert(IntelRendezvousConsumeBudget(
        INTEL_POLICY_EXIT_EXTERNAL_INTERRUPT, 8) == 8);
    assert(IntelRendezvousReloadBudget() == 8);
    return 0;
}
