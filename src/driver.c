#include <ntifs.h>

#include "hv.h"
#include "hv_log.h"

DRIVER_INITIALIZE DriverEntry;

#define JOHNSMITH_POOL_TAG_REGISTRY 'rSvJ'
#define JOHNSMITH_POOL_TAG_SECURITY 'sSvJ'
#define JOHNSMITH_START_SUCCEEDED 2u
#define JOHNSMITH_START_FAILED 3u

static HV_STATE* g_Hypervisor;
static UNICODE_STRING g_RegistryPath;

static NTSTATUS
JohnSmithOpenServiceKey(
    _In_ ACCESS_MASK DesiredAccess,
    _Out_ PHANDLE KeyHandle
    )
{
    OBJECT_ATTRIBUTES attributes;

    InitializeObjectAttributes(
        &attributes,
        &g_RegistryPath,
        OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
        NULL,
        NULL);
    return ZwOpenKey(KeyHandle, DesiredAccess, &attributes);
}

static NTSTATUS
JohnSmithOpenParametersKey(
    _In_ ACCESS_MASK DesiredAccess,
    _Out_ PHANDLE KeyHandle
    )
{
    UNICODE_STRING name = RTL_CONSTANT_STRING(L"Parameters");
    OBJECT_ATTRIBUTES attributes;
    HANDLE serviceKey;
    NTSTATUS status;

    status = JohnSmithOpenServiceKey(KEY_READ, &serviceKey);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    InitializeObjectAttributes(
        &attributes,
        &name,
        OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
        serviceKey,
        NULL);
    status = ZwOpenKey(KeyHandle, DesiredAccess, &attributes);
    ZwClose(serviceKey);
    return status;
}

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
JohnSmithQueryDword(
    _In_ PCWSTR ValueName,
    _Out_ PULONG Value
    )
{
    union {
        KEY_VALUE_PARTIAL_INFORMATION Information;
        UCHAR Buffer[
            FIELD_OFFSET(KEY_VALUE_PARTIAL_INFORMATION, Data) +
            sizeof(ULONG)];
    } valueInformation;
    UNICODE_STRING name;
    HANDLE keyHandle;
    ULONG resultLength;
    NTSTATUS status;

    RtlInitUnicodeString(&name, ValueName);
    status = JohnSmithOpenParametersKey(KEY_QUERY_VALUE, &keyHandle);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = ZwQueryValueKey(
        keyHandle,
        &name,
        KeyValuePartialInformation,
        &valueInformation,
        sizeof(valueInformation),
        &resultLength);
    if (NT_SUCCESS(status)) {
        if (valueInformation.Information.Type != REG_DWORD ||
            valueInformation.Information.DataLength != sizeof(ULONG)) {
            status = STATUS_OBJECT_TYPE_MISMATCH;
        } else {
            RtlCopyMemory(
                Value,
                valueInformation.Information.Data,
                sizeof(*Value));
        }
    }
    ZwClose(keyHandle);
    return status;
}

_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS
JohnSmithDeleteRegistryValue(
    _In_ PCWSTR ValueName
    )
{
    UNICODE_STRING name;
    HANDLE keyHandle;
    NTSTATUS status;

    RtlInitUnicodeString(&name, ValueName);
    status = JohnSmithOpenParametersKey(KEY_SET_VALUE, &keyHandle);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = ZwDeleteValueKey(keyHandle, &name);
    ZwClose(keyHandle);
    return status;
}

_IRQL_requires_(PASSIVE_LEVEL)
static NTSTATUS
JohnSmithRestrictServiceKeyAcl(
    VOID
    )
{
    SECURITY_DESCRIPTOR securityDescriptor;
    PACL dacl = NULL;
    HANDLE parametersKey = NULL;
    HANDLE serviceKey = NULL;
    ULONG daclLength;
    NTSTATUS status;

    if (SeExports == NULL ||
        SeExports->SeLocalSystemSid == NULL ||
        SeExports->SeAliasAdminsSid == NULL) {
        return STATUS_INVALID_DEVICE_STATE;
    }

    daclLength = sizeof(ACL) +
        2 * (sizeof(ACCESS_ALLOWED_ACE) - sizeof(ULONG)) +
        RtlLengthSid(SeExports->SeLocalSystemSid) +
        RtlLengthSid(SeExports->SeAliasAdminsSid);
    dacl = (PACL)ExAllocatePool2(
        POOL_FLAG_PAGED, daclLength, JOHNSMITH_POOL_TAG_SECURITY);
    if (dacl == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    status = RtlCreateAcl(dacl, daclLength, ACL_REVISION);
    if (NT_SUCCESS(status)) {
        status = RtlAddAccessAllowedAce(
            dacl,
            ACL_REVISION,
            KEY_ALL_ACCESS,
            SeExports->SeLocalSystemSid);
    }
    if (NT_SUCCESS(status)) {
        status = RtlAddAccessAllowedAce(
            dacl,
            ACL_REVISION,
            KEY_ALL_ACCESS,
            SeExports->SeAliasAdminsSid);
    }
    if (NT_SUCCESS(status)) {
        status = RtlCreateSecurityDescriptor(
            &securityDescriptor, SECURITY_DESCRIPTOR_REVISION);
    }
    if (NT_SUCCESS(status)) {
        status = RtlSetDaclSecurityDescriptor(
            &securityDescriptor, TRUE, dacl, FALSE);
    }
    if (NT_SUCCESS(status)) {
        status = JohnSmithOpenServiceKey(
            READ_CONTROL | WRITE_DAC, &serviceKey);
    }
    if (NT_SUCCESS(status)) {
        status = JohnSmithOpenParametersKey(
            READ_CONTROL | WRITE_DAC, &parametersKey);
    }
    if (NT_SUCCESS(status)) {
        status = ZwSetSecurityObject(
            parametersKey,
            (SECURITY_INFORMATION)(
                DACL_SECURITY_INFORMATION |
                PROTECTED_DACL_SECURITY_INFORMATION),
            &securityDescriptor);
    }
    if (NT_SUCCESS(status)) {
        status = ZwSetSecurityObject(
            serviceKey,
            (SECURITY_INFORMATION)(
                DACL_SECURITY_INFORMATION |
                PROTECTED_DACL_SECURITY_INFORMATION),
            &securityDescriptor);
    }

    if (parametersKey != NULL) {
        ZwClose(parametersKey);
    }
    if (serviceKey != NULL) {
        ZwClose(serviceKey);
    }
    ExFreePoolWithTag(dacl, JOHNSMITH_POOL_TAG_SECURITY);
    return status;
}

static VOID
JohnSmithSetStartResult(
    _In_ ULONG State,
    _In_ NTSTATUS Status
    )
{
    UNICODE_STRING stateName = RTL_CONSTANT_STRING(L"StartState");
    UNICODE_STRING statusName = RTL_CONSTANT_STRING(L"StartStatus");
    HANDLE keyHandle;

    if (!NT_SUCCESS(JohnSmithOpenServiceKey(KEY_SET_VALUE, &keyHandle))) {
        return;
    }
    (VOID)ZwSetValueKey(
        keyHandle, &stateName, 0, REG_DWORD, &State, sizeof(State));
    (VOID)ZwSetValueKey(
        keyHandle, &statusName, 0, REG_DWORD, &Status, sizeof(Status));
    ZwClose(keyHandle);
}

static VOID
JohnSmithFreeRegistryPath(
    VOID
    )
{
    if (g_RegistryPath.Buffer != NULL) {
        ExFreePoolWithTag(
            g_RegistryPath.Buffer, JOHNSMITH_POOL_TAG_REGISTRY);
        RtlZeroMemory(&g_RegistryPath, sizeof(g_RegistryPath));
    }
}

_IRQL_requires_(PASSIVE_LEVEL)
static VOID
JohnSmithUnload(
    _In_ PDRIVER_OBJECT DriverObject
    )
{
    UNREFERENCED_PARAMETER(DriverObject);

    if (g_Hypervisor != NULL) {
        HvStop(g_Hypervisor);
        g_Hypervisor = NULL;
    }
    JohnSmithFreeRegistryPath();
}

_Use_decl_annotations_
NTSTATUS
DriverEntry(
    PDRIVER_OBJECT DriverObject,
    PUNICODE_STRING RegistryPath
    )
{
    ULONG state;
    NTSTATUS status;

    DriverObject->DriverUnload = JohnSmithUnload;
    RtlZeroMemory(&g_RegistryPath, sizeof(g_RegistryPath));

    g_RegistryPath.MaximumLength = RegistryPath->Length + sizeof(WCHAR);
    g_RegistryPath.Buffer = (PWSTR)ExAllocatePool2(
        POOL_FLAG_NON_PAGED,
        g_RegistryPath.MaximumLength,
        JOHNSMITH_POOL_TAG_REGISTRY);
    if (g_RegistryPath.Buffer == NULL) {
        DriverObject->DriverUnload = NULL;
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(g_RegistryPath.Buffer, g_RegistryPath.MaximumLength);
    RtlCopyUnicodeString(&g_RegistryPath, RegistryPath);

    status = JohnSmithRestrictServiceKeyAcl();
    if (NT_SUCCESS(status)) {
        status = HvStart(&g_Hypervisor);
    }
    state = NT_SUCCESS(status)
        ? JOHNSMITH_START_SUCCEEDED
        : JOHNSMITH_START_FAILED;
    JohnSmithSetStartResult(state, status);

    if (!NT_SUCCESS(status)) {
        DriverObject->DriverUnload = NULL;
        g_Hypervisor = NULL;
        JohnSmithFreeRegistryPath();
    }

    return status;
}
