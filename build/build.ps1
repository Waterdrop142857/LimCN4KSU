<#
.SYNOPSIS
  Cross-compiles the bybus module with the bundled Android NDK (arm64-v8a).

.DESCRIPTION
  No cmake and no host compiler is assumed. Every artefact stays inside this
  project folder:
    build/obj/<group>/...    object files
    build/out/               linked binaries and the zygisk library
  Targets:
    libs      objects for shared + translation + vendored code
    zygisk    build/out/bybus-zygisk.so  (installed as zygisk/arm64-v8a.so)
    tools     build/out/bybusd and build/out/bybusctl
    all       libs + zygisk + tools
    host      host test binary (needs a host c++ compiler; prints a skip note otherwise)
    clean     removes build/obj and build/out
#>
param(
    [ValidateSet('all', 'libs', 'zygisk', 'tools', 'host', 'clean')]
    [string]$Target = 'all',
    [switch]$KeepGoing
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$ndk = if ($env:BYBUS_NDK) { $env:BYBUS_NDK } else { 'D:\BYBUS\android-ndk-r30' }
$toolchain = Join-Path $ndk 'toolchains\llvm\prebuilt\windows-x86_64\bin'
$api = '24'
$cxx = Join-Path $toolchain "aarch64-linux-android$api-clang++.cmd"
$cc = Join-Path $toolchain "aarch64-linux-android$api-clang.cmd"
$objRoot = Join-Path $PSScriptRoot 'obj'
$outDir = Join-Path $PSScriptRoot 'out'

# Our own code is strict: warnings are errors.
$commonFlags = @(
    '-std=c++17', '-O2', '-fPIC', '-fvisibility=hidden', '-ffunction-sections',
    '-fdata-sections', '-fno-omit-frame-pointer', '-Wall', '-Wextra', '-Werror',
    '-DANDROID', "-I$root\src", "-I$root\third_party\lzma-sdk",
    "-I$root\third_party\zygisk\jni", "-I$root\third_party\substrate"
)
$cFlags = @(
    '-O2', '-fPIC', '-ffunction-sections', '-fdata-sections', '-Wall', '-Wextra',
    '-DANDROID', "-I$root\third_party\lzma-sdk"
)
# Vendored upstream code is not reformatted; it is compiled quietly on purpose.
$vendorFlags = @(
    '-O2', '-fPIC', '-ffunction-sections', '-fdata-sections', '-w', '-DANDROID',
    "-I$root\third_party\lzma-sdk", "-I$root\third_party\substrate"
)

$sharedSources = @(
    'src/shared/status.cpp', 'src/shared/sha256.cpp', 'src/shared/utf.cpp',
    'src/shared/json.cpp', 'src/shared/fs.cpp'
)
$translationCore = @(
    'src/translation/text_policy.cpp', 'src/translation/json_pairer.cpp',
    'src/translation/snapshot.cpp'
)
$translationService = @(
    'src/translation/archive_reader.cpp', 'src/translation/archive_import.cpp',
    'src/translation/snapshot_builder.cpp', 'src/translation/elf_profile.cpp',
    'src/translation/appsealing_profile.cpp', 'src/translation/index_compiler.cpp'
)
$runtimeSources = @(
    'src/runtime/log.cpp', 'src/runtime/companion.cpp', 'src/runtime/snapshot_reader.cpp',
    'src/runtime/il2cpp.cpp', 'src/runtime/integrity.cpp', 'src/runtime/state.cpp',
    'src/runtime/appsealing.cpp', 'src/runtime/sigsegv_guard.cpp',
    'src/runtime/library_watch.cpp',
    'src/runtime/translation_index.cpp',
    'src/runtime/text_router.cpp', 'src/runtime/hooks.cpp', 'src/runtime/zygisk_module.cpp'
)
$substrateSources = @(
    'third_party/substrate/And64InlineHook.cpp', 'third_party/substrate/SubstrateHook.cpp',
    'third_party/substrate/SubstrateDebug.cpp', 'third_party/substrate/SubstratePosixMemory.cpp',
    'third_party/substrate/RuntimePage.cpp'
)
$lzmaSources = @(
    '7zArcIn.c', '7zDec.c', '7zCrc.c', '7zCrcOpt.c', '7zBuf.c', '7zBuf2.c', '7zStream.c',
    'CpuArch.c', 'LzmaDec.c', 'Lzma2Dec.c', 'Bra.c', 'Bra86.c', 'BraIA64.c', 'Bcj2.c',
    'Delta.c', 'Ppmd7.c', 'Ppmd7Dec.c'
)
$lzmaSources = @($lzmaSources | ForEach-Object { "third_party/lzma-sdk/$_" })
$serviceSources = @(Get-ChildItem -Path (Join-Path $root 'src\service') -Filter *.cpp -ErrorAction SilentlyContinue |
    ForEach-Object { "src/service/$($_.Name)" })
$controlSources = @(Get-ChildItem -Path (Join-Path $root 'src\control') -Filter *.cpp -ErrorAction SilentlyContinue |
    ForEach-Object { "src/control/$($_.Name)" })

function Get-ObjectPath([string]$group, [string]$source) {
    $name = [System.IO.Path]::GetFileNameWithoutExtension($source) + '.o'
    $relative = [System.IO.Path]::GetDirectoryName($source) -replace '[\\/]', '_'
    $dir = Join-Path (Join-Path $objRoot $group) $relative
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    Join-Path $dir $name
}

$script:failures = @()

function Invoke-Compile {
    param([string]$Group, [string]$Source, [switch]$IsC, [switch]$Vendor)
    $absolute = Join-Path $root $Source
    if (-not (Test-Path $absolute)) { throw "missing source: $Source" }
    $object = Get-ObjectPath $Group $Source
    $compiler = if ($IsC) { $cc } else { $cxx }
    $flags = if ($Vendor) { $vendorFlags } elseif ($IsC) { $cFlags } else { $commonFlags }
    $previousErrorAction = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & $compiler @flags -c $absolute -o $object 2>&1 | ForEach-Object { Write-Host $_ }
    } finally {
        $ErrorActionPreference = $previousErrorAction
    }
    if ($LASTEXITCODE -ne 0) {
        $script:failures += $Source
        if (-not $KeepGoing) { throw "compile failed: $Source" }
        return $null
    }
    return $object
}

function Compile-Group {
    param([string]$Group, [string[]]$Sources, [switch]$IsC, [switch]$Vendor)
    $objects = @()
    foreach ($source in $Sources) {
        $object = Invoke-Compile -Group $Group -Source $source -IsC:$IsC -Vendor:$Vendor
        if ($object) { $objects += $object }
    }
    return $objects
}

function Link-Executable {
    param([string]$Name, [string[]]$Objects)
    New-Item -ItemType Directory -Force -Path $outDir | Out-Null
    $output = Join-Path $outDir $Name
    $previousErrorAction = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & $cxx -o $output @Objects '-static-libstdc++' '-Wl,--gc-sections' '-llog' '-lz' '-ldl' 2>&1 |
            ForEach-Object { Write-Host $_ }
    } finally {
        $ErrorActionPreference = $previousErrorAction
    }
    if ($LASTEXITCODE -ne 0) { throw "link failed: $Name" }
    Write-Host "linked $output"
    return $output
}

function Link-Shared {
    param([string]$Name, [string[]]$Objects)
    New-Item -ItemType Directory -Force -Path $outDir | Out-Null
    $output = Join-Path $outDir $Name
    $previousErrorAction = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & $cxx -shared -o $output @Objects '-static-libstdc++' '-Wl,--gc-sections' '-llog' '-ldl' 2>&1 |
            ForEach-Object { Write-Host $_ }
    } finally {
        $ErrorActionPreference = $previousErrorAction
    }
    if ($LASTEXITCODE -ne 0) { throw "link failed: $Name" }
    Write-Host "linked $output"
    return $output
}

if ($Target -eq 'clean') {
    Remove-Item -Recurse -Force $objRoot, $outDir -ErrorAction SilentlyContinue
    Write-Host 'cleaned'
    return
}
if ($Target -eq 'host') {
    & (Join-Path $PSScriptRoot 'test-host.ps1')
    if ($LASTEXITCODE -ne 0) { throw 'host smoke test failed' }
    return
}

Write-Host "objective: $Target  ndk: $ndk"
$sharedObjects = Compile-Group -Group 'shared' -Sources ($sharedSources + $translationCore)

$vendorObjects = @()
$substrateObjects = @()
$translationObjects = @()
if ($Target -in @('all', 'libs', 'zygisk', 'tools')) {
    $vendorObjects = Compile-Group -Group 'lzma' -Sources $lzmaSources -IsC -Vendor
    $substrateObjects = Compile-Group -Group 'substrate' -Sources $substrateSources -Vendor
    $translationObjects = Compile-Group -Group 'translation' -Sources $translationService
}

if ($Target -in @('all', 'zygisk')) {
    $runtimeObjects = Compile-Group -Group 'runtime' -Sources $runtimeSources
    Link-Shared -Name 'bybus-zygisk.so' -Objects ($runtimeObjects + $sharedObjects + $substrateObjects) |
        Out-Null
}

if ($Target -in @('all', 'tools')) {
    $serviceObjects = Compile-Group -Group 'service' -Sources $serviceSources
    $controlObjects = Compile-Group -Group 'control' -Sources $controlSources
    $serviceMain = @($serviceObjects | Where-Object { $_ -match 'bybusd_main' })
    $others = @($serviceObjects | Where-Object { $_ -notmatch 'bybusd_main' })
    if ($serviceMain.Count -ne 1) { throw 'bybusd_main.cpp is required exactly once' }
    if ($controlObjects.Count -eq 0) { throw 'bybusctl source is missing' }
    Link-Executable -Name 'bybusd' `
        -Objects ($others + $serviceMain + $sharedObjects + $translationObjects + $vendorObjects) |
        Out-Null
    Link-Executable -Name 'bybusctl' -Objects (@($controlObjects) + @($sharedObjects)) |
        Out-Null
}

if ($script:failures.Count -gt 0) {
    Write-Host "failed sources:`n$($script:failures -join "`n")"
    exit 1
}
Write-Host 'done'
