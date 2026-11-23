param()
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$hostEnv = if ($env:BYBUS_HOST_ENV) { $env:BYBUS_HOST_ENV } else {
    'D:\MarkNext\.marknext-dev\env.ps1'
}
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue) -and (Test-Path $hostEnv)) {
    . $hostEnv
}
$compiler = Get-Command cl.exe -ErrorAction SilentlyContinue
if (-not $compiler) { throw 'MSVC cl.exe not found; set BYBUS_HOST_ENV' }
$out = Join-Path $PSScriptRoot 'out'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$sources = @(
    'tests/host_smoke.cpp',
    'src/shared/json.cpp', 'src/shared/sha256.cpp', 'src/shared/utf.cpp',
    'src/translation/text_policy.cpp', 'src/translation/json_pairer.cpp',
    'src/translation/snapshot.cpp', 'src/translation/appsealing_profile.cpp',
    'src/translation/index_compiler.cpp', 'src/runtime/translation_index.cpp'
) | ForEach-Object { Join-Path $root $_ }
Push-Location $out
try {
    & $compiler.Source /nologo /std:c++17 /EHsc /utf-8 /W4 /WX /O2 "/I$root\src" @sources /Fe:bybus-host-smoke.exe
    if ($LASTEXITCODE -ne 0) { throw 'host test compile failed' }
    & (Join-Path $out 'bybus-host-smoke.exe')
    if ($LASTEXITCODE -ne 0) { throw 'host smoke test failed' }
    Write-Host 'host smoke passed'
} finally {
    Pop-Location
}
