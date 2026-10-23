$ErrorActionPreference = 'Stop'

$workspace = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$toolsDir = Join-Path $workspace 'recomp\.tools'
$rustDir = Join-Path $toolsDir 'rust'
$nodDir = Join-Path $toolsDir 'nod-source'
$nodCommit = '6a54b418e541ce5eeb364399116afd6e47b817bb'
$iso = Join-Path $workspace 'Metroid Prime 3 - Corruption (USA).iso'
$extraction = Join-Path $toolsDir 'disc-extraction-rm3e01'
$source = Join-Path $PSScriptRoot 'nod_iso_partition_probe.cpp'
$headerDir = Join-Path $nodDir 'nod-ffi\include'
$library = Join-Path $nodDir 'target\x86_64-pc-windows-gnu\release\libnod.a'
$cargo = Join-Path $rustDir 'cargo-home\bin\cargo.exe'
$clang = Join-Path $toolsDir 'portable-tools\llvm-mingw\bin\clang++.exe'
$output = Join-Path $toolsDir 'nod_iso_partition_probe.exe'

if (-not (Test-Path -LiteralPath $nodDir)) {
    git clone --filter=blob:none --no-checkout https://github.com/encounter/nod.git $nodDir
    if ($LASTEXITCODE -ne 0) { throw 'Could not clone the pinned nod source.' }
}
$workingChanges = git -C $nodDir status --porcelain
if ($LASTEXITCODE -ne 0) { throw 'Could not inspect the nod source tree.' }
if ($workingChanges) { throw 'The local nod source tree has modifications; refusing to build unpinned code.' }
git -C $nodDir checkout --detach $nodCommit
if ($LASTEXITCODE -ne 0) { throw 'Could not select the pinned nod commit.' }
$actualCommit = (git -C $nodDir rev-parse HEAD).Trim()
if ($actualCommit -ne $nodCommit) { throw "Unexpected nod commit: $actualCommit" }

foreach ($required in @($cargo, $clang, $source, $iso, (Join-Path $extraction 'sys\main.dol'))) {
    if (-not (Test-Path -LiteralPath $required)) { throw "Required input is missing: $required" }
}
$isoBefore = Get-Item -LiteralPath $iso
if (($isoBefore.Attributes -band [IO.FileAttributes]::ReadOnly) -eq 0) {
    throw 'The supplied ISO must remain read-only.'
}

$env:RUSTUP_HOME = Join-Path $rustDir 'rustup-home'
$env:CARGO_HOME = Join-Path $rustDir 'cargo-home'
$rustVersion = (& (Join-Path $rustDir 'cargo-home\bin\rustc.exe') --version).Trim()
if ($rustVersion -ne 'rustc 1.99.1 (58cf8f9c3 2026-10-13)') {
    throw "Unexpected Rust toolchain: $rustVersion"
}

& $cargo build --locked --release --target x86_64-pc-windows-gnu `
    --manifest-path (Join-Path $nodDir 'nod-ffi\Cargo.toml') `
    --no-default-features --features threading
if ($LASTEXITCODE -ne 0) { throw 'The pinned nod FFI crate did not build.' }

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
Write-Output "rust_toolchain=$rustVersion"
Write-Output 'iso_metadata_unchanged=true'
