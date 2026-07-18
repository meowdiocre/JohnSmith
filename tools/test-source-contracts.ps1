$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot

function Read-RepoFile([string]$Path) {
    Get-Content -LiteralPath (Join-Path $root $Path) -Raw
}

function Assert-Match([string]$Text, [string]$Pattern, [string]$Message) {
    if ($Text -notmatch $Pattern) {
        throw $Message
    }
}

$kernel = Read-RepoFile 'src/intel/intel_internal.h'
$client = Read-RepoFile 'tools/johnsmithctl/common/types.h'
$main = Read-RepoFile 'tools/johnsmithctl/main.cpp'
$loader = Read-RepoFile 'tools/johnsmithctl/loader/loader.h'
$project = Read-RepoFile 'tools/johnsmithctl/johnsmithctl.vcxproj'
$solution = Read-RepoFile 'JohnSmith.sln'
$readme = Read-RepoFile 'README.md'

$commands = @(
    @('REGISTER', 0),
    @('INSTALL', 1),
    @('REMOVE', 2),
    @('READ', 3),
    @('WRITE', 4),
    @('QUERY_HOOK', 5),
    @('LIST_HOOKS', 6),
    @('PROBE', 7),
    @('COUNT', 8)
)

foreach ($command in $commands) {
    Assert-Match $kernel ("INTEL_HYPERCALL_CMD_{0}\s*=\s*{1}" -f $command[0], $command[1]) "kernel command mismatch: $($command[0])"
}

Assert-Match $client '(?s)enum Command.*CommandRegister\s*,.*CommandInstall\s*,.*CommandRemove\s*,.*CommandRead\s*,.*CommandWrite\s*,.*CommandQueryHook\s*,.*CommandListHooks\s*,.*CommandProbe\s*,.*CommandCount' 'client command order changed'

Assert-Match $kernel 'INTEL_HCALL_PAYLOAD_OFFSET\s+256u' 'kernel payload offset changed'
Assert-Match $kernel 'C_ASSERT\(sizeof\(INTEL_HCALL_PAGE\) == 4096\)' 'kernel page size assertion missing'
Assert-Match $client 'kPayloadOffset\s*=\s*256u' 'client payload offset changed'
Assert-Match $client 'static_assert\(sizeof\(HypercallPage\) == 4096\)' 'client page size assertion missing'
Assert-Match $project 'ProjectConfiguration Include="Debug\|x64"' 'johnsmithctl Debug configuration missing'
Assert-Match $project 'ProjectConfiguration Include="Benchmark\|x64"' 'johnsmithctl Benchmark configuration missing'
Assert-Match $project 'build\\bin\\\$\(Configuration\)\\JohnSmith\.sys' 'tool packaging ignores its configuration'
Assert-Match $project '<KduConfiguration>Release</KduConfiguration>' 'KDU configuration default missing'
Assert-Match $project 'KDU\\Source\\KDU\.sln' 'johnsmithctl does not build the KDU solution'
if ($project -match 'KDU\.sln.*\/t:') {
    throw 'ambiguous KDU solution target list remains'
}
Assert-Match $solution '\{9A3244F8-4F17-4C4D-A4C6-7E9D8278A1A2\}\.Debug\|x64\.ActiveCfg = Debug\|x64' 'solution does not map johnsmithctl Debug'
Assert-Match $solution '\{9A3244F8-4F17-4C4D-A4C6-7E9D8278A1A2\}\.Benchmark\|x64\.ActiveCfg = Benchmark\|x64' 'solution does not map johnsmithctl Benchmark'

if ($main -match '--config' -or $loader -match 'const wchar_t\* config') {
    throw 'dead loader configuration option remains'
}
if ($main -match "argv\[i\]\[0\] != '-'") {
    throw 'unknown start options are silently ignored'
}

if ($readme -match 'LLM-assisted|AI-generated') {
    throw 'README contains AI attribution'
}
$plans = Get-ChildItem -LiteralPath (Join-Path $root 'docs/superpowers') -Recurse -File -ErrorAction SilentlyContinue
if ($plans) {
    throw 'generated planning files remain'
}

$documentation = @(
    Read-RepoFile 'README.md'
    Read-RepoFile 'DOCUMENTATION.md'
)
$documentation += Get-ChildItem -LiteralPath (Join-Path $root 'docs') -Recurse -File -Filter '*.md' |
    ForEach-Object { Get-Content -LiteralPath $_.FullName -Raw }
$documentationText = $documentation -join "`n"
$obsoleteDocumentation = @(
    'IOCTL',
    'johnsmith_ioctl',
    '\\Device\\JohnSmith',
    'load-kdu\.ps1',
    'unload-kdu\.ps1',
    'StartRequested',
    'ept-roadmap'
)
foreach ($pattern in $obsoleteDocumentation) {
    if ($documentationText -match $pattern) {
        throw "obsolete documentation reference remains: $pattern"
    }
}
if (Test-Path -LiteralPath (Join-Path $root 'docs/ept-roadmap.md')) {
    throw 'speculative EPT roadmap remains'
}

$debugMarkers = Get-ChildItem -LiteralPath (Join-Path $root 'src') -Recurse -File -Filter '*.c' |
    Select-String -SimpleMatch 'stage='
if ($debugMarkers) {
    throw "debug stage markers remain: $($debugMarkers.Path -join ', ')"
}

Write-Output 'source contracts: PASS'
