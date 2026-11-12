# Prepares a fresh workspace for building (Windows). Run from anywhere:
#   powershell -ExecutionPolicy Bypass -File recomp\tools\setup_workspace.ps1 [-Extraction <dir>]
# 1. Copies the support files of recomp\workspace into the workspace root (never overwrites).
# 2. With an extraction made from your own disc (sys\main.dol, files\RSO\...), links it where the
#    scripts expect it (directory junctions) and copies main.dol into recomp\input\RM3E01_00 after
#    checking its hash.
param([string]$Extraction)
$ErrorActionPreference = 'Stop'
$recomp = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$root = (Resolve-Path (Join-Path $recomp '..')).Path
$support = Join-Path $recomp 'workspace'
Get-ChildItem -LiteralPath $support -Recurse -File | Where-Object { $_.Name -ne 'README.md' } | ForEach-Object {
    $relative = $_.FullName.Substring($support.Length + 1)
    $target = Join-Path $root $relative
    if (Test-Path -LiteralPath $target) { Write-Host "kept     $target" }
    else {
        New-Item -ItemType Directory -Force -Path (Split-Path $target) | Out-Null
        Copy-Item -LiteralPath $_.FullName -Destination $target
        Write-Host "copied   $target"
    }
}
if (-not $Extraction) { Write-Host 'No extraction given: skipping the disc inputs.'; exit 0 }
$Extraction = (Resolve-Path $Extraction).Path
$dol = Join-Path $Extraction 'sys\main.dol'
if (-not (Test-Path -LiteralPath $dol)) { throw "$dol not found" }
$hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $dol).Hash.ToLowerInvariant()
if ($hash -ne '6b550f221602074747a2e61b0aa064203fd493f6865dfb3b1a912682065e6104') {
    throw 'main.dol is not Metroid Prime 3 Corruption USA (RM3E01) revision 0'
}
$inputDir = Join-Path $recomp 'input\RM3E01_00'
New-Item -ItemType Directory -Force -Path $inputDir, (Join-Path $recomp '.tools'), (Join-Path $root 'decomp\orig') | Out-Null
if (-not (Test-Path -LiteralPath (Join-Path $inputDir 'main.dol'))) { Copy-Item -LiteralPath $dol -Destination $inputDir }
foreach ($link in @((Join-Path $recomp '.tools\disc-extraction-rm3e01'), (Join-Path $root 'decomp\orig\RM3E01_00'))) {
    if (-not (Test-Path -LiteralPath $link)) { New-Item -ItemType Junction -Path $link -Target $Extraction | Out-Null }
    Write-Host "linked   $link -> $Extraction"
}
