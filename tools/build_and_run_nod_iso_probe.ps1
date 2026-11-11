$ErrorActionPreference = 'Stop'

$workspace = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$toolsDir = Join-Path $workspace 'recomp\.tools'
$nodDir = Join-Path $toolsDir 'nod-source'
$iso = Join-Path $workspace 'Metroid Prime 3 - Corruption (USA).iso'
$extraction = Join-Path $toolsDir 'disc-extraction-rm3e01'
$source = Join-Path $PSScriptRoot 'nod_iso_partition_probe.cpp'
$headerDir = Join-Path $nodDir 'nod-ffi\include'
$library = Join-Path $nodDir 'target\x86_64-pc-windows-gnu\release\libnod.a'
$clang = Join-Path $toolsDir 'portable-tools\llvm-mingw\bin\clang++.exe'
$output = Join-Path $toolsDir 'nod_iso_partition_probe.exe'

& (Join-Path $PSScriptRoot 'build_nod_ffi.ps1')
if ($LASTEXITCODE -ne 0 -and $null -ne $LASTEXITCODE) { throw 'The pinned nod FFI crate did not build.' }
$actualCommit = (git -C $nodDir rev-parse HEAD).Trim()

foreach ($required in @($clang, $source, $iso, (Join-Path $extraction 'sys\main.dol'))) {
    if (-not (Test-Path -LiteralPath $required)) { throw "Required input is missing: $required" }
}
$isoBefore = Get-Item -LiteralPath $iso
if (($isoBefore.Attributes -band [IO.FileAttributes]::ReadOnly) -eq 0) {
    throw 'The supplied ISO must remain read-only.'
}

$env:PATH = (Join-Path $toolsDir 'portable-tools\llvm-mingw\bin') + ';' + $env:PATH
& $clang -std=c++20 -O2 -I $headerDir $source $library `
    -lbcrypt -luserenv -lws2_32 -ladvapi32 -luser32 -lkernel32 -lshell32 -lole32 -lntdll `
    -o $output
if ($LASTEXITCODE -ne 0) { throw 'The C++ probe did not link against nod-ffi.' }

& $output $iso $extraction
if ($LASTEXITCODE -ne 0) { throw 'The direct ISO partition comparison failed.' }
$isoAfter = Get-Item -LiteralPath $iso
if ($isoAfter.Length -ne $isoBefore.Length -or
    $isoAfter.LastWriteTimeUtc -ne $isoBefore.LastWriteTimeUtc -or
    ($isoAfter.Attributes -band [IO.FileAttributes]::ReadOnly) -eq 0) {
    throw 'ISO metadata changed during the read-only probe.'
}
Write-Output "nod_commit=$actualCommit"
Write-Output 'iso_metadata_unchanged=true'
