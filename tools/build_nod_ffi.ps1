$ErrorActionPreference = 'Stop'

# Builds the pinned nod FFI archive the runtime links. The compressors are not
# optional: without them .rvz, .wia and .gcz images fail when opened.
$workspace = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$toolsDir = Join-Path $workspace 'recomp\.tools'
$rustDir = Join-Path $toolsDir 'rust'
$nodDir = Join-Path $toolsDir 'nod-source'
$nodCommit = '6a54b418e541ce5eeb364399116afd6e47b817bb'
$features = 'threading,compress-zstd-static,compress-zlib-static,compress-bzip2-static,compress-lzma-static'
$cargo = Join-Path $rustDir 'cargo-home\bin\cargo.exe'
$llvmBin = Join-Path $toolsDir 'portable-tools\llvm-mingw\bin'
$selfContained = Join-Path $rustDir 'rustup-home\toolchains\stable-x86_64-pc-windows-gnu\lib\rustlib\x86_64-pc-windows-gnu\bin\self-contained'

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

foreach ($required in @($cargo, (Join-Path $llvmBin 'clang.exe'), (Join-Path $selfContained 'x86_64-w64-mingw32-gcc.exe'))) {
    if (-not (Test-Path -LiteralPath $required)) { throw "Required input is missing: $required" }
}

$env:RUSTUP_HOME = Join-Path $rustDir 'rustup-home'
$env:CARGO_HOME = Join-Path $rustDir 'cargo-home'
$rustVersion = (& (Join-Path $rustDir 'cargo-home\bin\rustc.exe') --version).Trim()
if ($rustVersion -ne 'rustc 1.99.1 (58cf8f9c3 2026-10-13)') {
    throw "Unexpected Rust toolchain: $rustVersion"
}

# Rust links with its own self-contained gcc (llvm-mingw's has no libgcc), but
# its GNU dlltool needs an assembler it does not ship, so dlltool comes from
# llvm-mingw. The C compressors build with llvm-mingw's clang.
$env:PATH = $llvmBin + ';' + $env:PATH
$env:CARGO_TARGET_X86_64_PC_WINDOWS_GNU_LINKER = Join-Path $selfContained 'x86_64-w64-mingw32-gcc.exe'
$env:CC_x86_64_pc_windows_gnu = Join-Path $llvmBin 'clang.exe'
$env:AR_x86_64_pc_windows_gnu = Join-Path $llvmBin 'llvm-ar.exe'

& $cargo build --locked --release --target x86_64-pc-windows-gnu `
    --manifest-path (Join-Path $nodDir 'nod-ffi\Cargo.toml') `
    --no-default-features --features $features
if ($LASTEXITCODE -ne 0) { throw 'The pinned nod FFI crate did not build.' }

# build_prime_runtime.ps1 refuses an archive without this record.
$releaseDir = Join-Path $nodDir 'target\x86_64-pc-windows-gnu\release'
[IO.File]::WriteAllText((Join-Path $releaseDir 'libnod.features.txt'), "$features`n")
Write-Output "nod_commit=$actualCommit"
Write-Output "rust_toolchain=$rustVersion"
Write-Output "nod_features=$features"
