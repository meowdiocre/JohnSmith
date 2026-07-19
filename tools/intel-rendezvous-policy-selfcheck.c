#define _CRT_SECURE_NO_WARNINGS

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/intel/intel_rendezvous_policy.h"

static char*
ReadTextFile(
    const char* Path
    )
{
    FILE* file;
    long length;
    size_t bytesRead;
    char* text;

    file = fopen(Path, "rb");
    assert(file != NULL);
    assert(fseek(file, 0, SEEK_END) == 0);
    length = ftell(file);
    assert(length >= 0);
    rewind(file);
    text = malloc((size_t)length + 1);
    assert(text != NULL);
    bytesRead = fread(text, 1, (size_t)length, file);
    assert(bytesRead == (size_t)length);
    assert(fclose(file) == 0);
    text[bytesRead] = '\0';
    return text;
}

int
main(void)
{
    unsigned budget;
    unsigned index;
    char* exitSource;
    char* completion;
    char* completionFinish;
    char* completionJoin;
    char* completionDiagnostics;
    char* flush;
    char* entryJoin;
    char* classify;
    char* begin;
    char* rdtsc;
    char* tscOffset;
    char* vmcsTscOffset;
    char* physicalNmi;
    char* expectedNmi;
    char* interruptibility;
    char* rdmsr;
    char* tscMsr;
    char* tscValue;
    char* featureControl;
    char* hookLookup;
    char* hookReload;
    char* hookTarget;

    assert(INTEL_RENDEZVOUS_ICR_LOW == 0x000C4400u);
    assert(IntelRendezvousClassifyPolicy(
        INTEL_POLICY_EXIT_CPUID, 0, 0) == INTEL_POLICY_MANDATORY);
    assert(IntelRendezvousClassifyPolicy(
        INTEL_POLICY_EXIT_RDTSC, 0, 0) == INTEL_POLICY_MANDATORY);
    assert(IntelRendezvousClassifyPolicy(
        INTEL_POLICY_EXIT_EPT_VIOLATION, 0, 0) ==
        INTEL_POLICY_MANDATORY);
    assert(IntelRendezvousClassifyPolicy(
        INTEL_POLICY_EXIT_RDTSCP, 0, 0) == INTEL_POLICY_MANDATORY);
    assert(IntelRendezvousClassifyPolicy(
        INTEL_POLICY_EXIT_RDMSR, INTEL_POLICY_IA32_TSC, 0) ==
        INTEL_POLICY_MANDATORY);
    assert(IntelRendezvousClassifyPolicy(
        INTEL_POLICY_EXIT_RDMSR, 0, 1) == INTEL_POLICY_CONDITIONAL);
    assert(IntelRendezvousClassifyPolicy(
        INTEL_POLICY_EXIT_RDMSR, 0, 0) == INTEL_POLICY_NONE);
    assert(IntelRendezvousClassifyPolicy(
        INTEL_POLICY_EXIT_CR_ACCESS, 0, 1) == INTEL_POLICY_CONDITIONAL);
    assert(IntelRendezvousClassifyPolicy(
        INTEL_POLICY_EXIT_CR_ACCESS, 0, 0) == INTEL_POLICY_NONE);
    assert(IntelRendezvousClassifyPolicy(
        INTEL_POLICY_EXIT_WRMSR, 0, 1) == INTEL_POLICY_CONDITIONAL);
    assert(IntelRendezvousClassifyPolicy(
        INTEL_POLICY_EXIT_WRMSR, 0, 0) == INTEL_POLICY_NONE);
    assert(IntelRendezvousClassifyPolicy(
        INTEL_POLICY_EXIT_GDTR_IDTR, 0, 1) == INTEL_POLICY_CONDITIONAL);
    assert(IntelRendezvousClassifyPolicy(
        INTEL_POLICY_EXIT_LDTR_TR, 0, 1) == INTEL_POLICY_CONDITIONAL);
    assert(IntelRendezvousClassifyPolicy(
        INTEL_POLICY_EXIT_EXTERNAL_INTERRUPT, 0, 8) ==
        INTEL_POLICY_EXCLUDED);
    assert(IntelRendezvousClassifyPolicy(
        INTEL_POLICY_EXIT_MTF, 0, 8) == INTEL_POLICY_EXCLUDED);
    assert(IntelRendezvousClassifyPolicy(
        INTEL_POLICY_EXIT_PREEMPTION_TIMER, 0, 8) ==
        INTEL_POLICY_EXCLUDED);
    assert(IntelRendezvousClassifyPolicy(
        0xFFFFFFFFu, 0, 8) == INTEL_POLICY_NONE);

    budget = INTEL_RENDEZVOUS_HOOK_BUDGET;
    for (index = 0; index < INTEL_RENDEZVOUS_HOOK_BUDGET; ++index) {
        budget = IntelRendezvousConsumeBudget(
            INTEL_POLICY_EXIT_CPUID, budget);
        assert(budget == INTEL_RENDEZVOUS_HOOK_BUDGET - index - 1);
    }
    assert(budget == 0);
    assert(IntelRendezvousConsumeBudget(
        INTEL_POLICY_EXIT_CPUID, budget) == 0);
    assert(IntelRendezvousConsumeBudget(
        INTEL_POLICY_EXIT_EXTERNAL_INTERRUPT, 8) == 8);
    assert(IntelRendezvousConsumeBudget(
        INTEL_POLICY_EXIT_MTF, 8) == 8);
    assert(IntelRendezvousConsumeBudget(
        INTEL_POLICY_EXIT_PREEMPTION_TIMER, 8) == 8);
    assert(IntelRendezvousReloadBudget() == 8);
    assert((INTEL_POLICY_REQUIRED_PIN_CONTROLS & (1u << 3)) != 0);
    assert((INTEL_POLICY_REQUIRED_PIN_CONTROLS & (1u << 5)) != 0);
    assert((INTEL_POLICY_REQUIRED_PRIMARY_CONTROLS & (1u << 3)) != 0);

    exitSource = ReadTextFile("src/intel/intel_exit.c");
    completion = strstr(exitSource, "IntelCompleteVmExit(");
    assert(completion != NULL);
    completionFinish = strstr(
        completion, "IntelRendezvousFinish(Context);");
    completionJoin = strstr(
        completion, "IntelRendezvousJoinActive(Context);");
    completionDiagnostics = strstr(
        completion, "#if JOHNSMITH_DIAGNOSTICS");
    assert(completionFinish != NULL);
    assert(completionJoin != NULL);
    assert(completionDiagnostics != NULL);
    assert(completionFinish < completionJoin);
    assert(completionJoin < completionDiagnostics);

    flush = strstr(exitSource, "IntelFlushEptIfNeeded(context);");
    assert(flush != NULL);
    entryJoin = strstr(flush, "IntelRendezvousJoinActive(context);");
    classify = strstr(
        flush, "rendezvousPolicy = IntelRendezvousClassifyAndConsume(");
    begin = strstr(flush, "IntelRendezvousBegin(context);");
    assert(entryJoin != NULL);
    assert(classify != NULL);
    assert(begin != NULL);
    assert(entryJoin < classify);
    assert(classify < begin);

    rdtsc = strstr(exitSource, "if (reason == VMX_EXIT_RDTSC ||");
    physicalNmi = strstr(exitSource, "if (reason == VMX_EXIT_EXCEPTION_OR_NMI)");
    assert(rdtsc != NULL);
    assert(physicalNmi != NULL);
    tscOffset = strstr(rdtsc, "ULONG64 tscOffset = context->TscOffset;");
    vmcsTscOffset = strstr(rdtsc, "VMCS_TSC_OFFSET");
    assert(tscOffset != NULL);
    assert(tscOffset < physicalNmi);
    assert(vmcsTscOffset == NULL || vmcsTscOffset > physicalNmi);
    expectedNmi = strstr(
        physicalNmi, "IntelRendezvousConsumeExpectedNmi(context)");
    interruptibility = strstr(
        physicalNmi, "VMCS_GUEST_INTERRUPTIBILITY");
    assert(expectedNmi != NULL);
    assert(interruptibility != NULL);
    assert(expectedNmi < interruptibility);

    rdmsr = strstr(exitSource, "if (reason == VMX_EXIT_RDMSR)");
    assert(rdmsr != NULL);
    tscMsr = strstr(rdmsr, "if (msr == IA32_TIME_STAMP_COUNTER)");
    featureControl = strstr(
        rdmsr, "else if (msr == IA32_FEATURE_CONTROL)");
    assert(tscMsr != NULL);
    assert(featureControl != NULL);
    assert(tscMsr < featureControl);
    tscValue = strstr(tscMsr, "__rdtsc() + context->TscOffset");
    assert(tscValue != NULL);
    assert(tscValue < featureControl);

    hookLookup = strstr(exitSource,
        "if (IntelHookLookup(backend, guestPhysical, &policy)) {");
    assert(hookLookup != NULL);
    hookReload = strstr(
        hookLookup, "IntelRendezvousReloadHookBudget(context);");
    hookTarget = strstr(
        hookLookup, "const INTEL_CPU_EPT_VIEW* target = NULL;");
    assert(hookReload != NULL);
    assert(hookTarget != NULL);
    assert(hookReload < hookTarget);
    free(exitSource);
    return 0;
}
