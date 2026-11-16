<#
 Private test package (not a release, never uploaded): the public build (QA tools off, no rate probe) packaged
 into dist\private_<Label>\, apart from the release packages. Afterwards the local test build is restored
 (QA tools on, rate probe back) unless -KeepPublicBuild.
   make_private_test_build.ps1 -Label native120
#>
param([string]$Label = 'test', [switch]$KeepPublicBuild)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$recomp = Join-Path $root 'recomp'
$runtime = Join-Path $recomp 'third_party\WiiCompiled\runtime'
$buildDir = Join-Path $recomp 'build\RM3E01_00_runtime_link_probe'
$tools = Join-Path $recomp '.tools\portable-tools'
$env:PATH = (Join-Path $tools 'llvm-mingw\bin') + ';' + (Join-Path $tools 'Ninja') + ';' + $env:PATH
$probe = Join-Path $recomp 'config\RM3E01_00\qa_rate_probe.json'
$probeAside = "$probe.private_build_aside"
$out = Join-Path $root "dist\private_$Label"

function Native([scriptblock]$Command) {
    $previous = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
    try { & $Command 2>&1 | Out-String | Write-Host } finally { $ErrorActionPreference = $previous }
    if ($LASTEXITCODE -ne 0) { throw "failed: $Command" }
}
function Configure([string]$qa) { Native { cmake -S $runtime -B $buildDir "-DMKW_PRIME_QA_TOOLS=$qa" } }
function FullBuild { Native { powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'build_prime_runtime.ps1') -Jobs 4 } }

if (@(Get-Process | Where-Object { $_.ProcessName -like 'Corruption_3*' }).Count) { throw 'Close the game first.' }
try {
    if (Test-Path $probe) { Move-Item $probe $probeAside -Force }
    Configure 'OFF'
    FullBuild
    $exe = Join-Path $buildDir 'Corruption_3 PC.exe'
    $text = [IO.File]::ReadAllText($exe, [Text.Encoding]::GetEncoding(28591))
    if ($text.Contains('MKW_PRIME_QA_TOOLS_ENABLED_BUILD')) { throw 'the executable still carries the QA tools' }
    if ($text.Contains('[rate-probe]')) { throw 'the executable still carries the rate probe' }
    New-Item -ItemType Directory -Force -Path $out | Out-Null
    Native { powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'package_release.ps1') -OutputDirectory "dist\private_$Label" }  # relative to the repository root
    Get-ChildItem $out -Filter *.zip | ForEach-Object {
        $named = Join-Path $out (($_.BaseName -replace '-win64$', "-$Label-PRIVATE-win64") + '.zip')
        Move-Item $_.FullName $named -Force
        Remove-Item "$($_.FullName).sha256" -ErrorAction SilentlyContinue
        "PRIVATE PACKAGE: $named"
        "sha256: " + (Get-FileHash $named -Algorithm SHA256).Hash.ToLower()
    }
} finally {
    if (Test-Path $probeAside) { Move-Item $probeAside $probe -Force }
    if (-not $KeepPublicBuild) {
        Configure 'ON'
        FullBuild
        'test build restored (QA tools on, rate probe back)'
    }
}
