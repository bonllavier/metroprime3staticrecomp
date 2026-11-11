<#
 Builds a public release of Corruption_3 PC end to end and reports the status of every step.
 It never uploads: publishing stays a separate, deliberate command (docs/publicar_version.md).

   make_release.ps1                    # package the version currently in prime_port_version.h
   make_release.ps1 -Bump patch        # 0.1.3 -> 0.1.4, then package
   make_release.ps1 -Version 0.2.0     # set that exact version, then package
   make_release.ps1 -SkipScan          # no VirusTotal lookup
   make_release.ps1 -KeepQaBuild       # leave the local build without the QA tools
   make_release.ps1 -AllowFlags        # do not stop when VirusTotal flags a file

 Steps: version, changelog check, close the game, build without the QA tools, package, VirusTotal,
 verify the zip, restore the QA build. Each one prints OK or FAIL; a failure stops the rest.
 The VirusTotal key is read from VIRUSTOTAL_API_KEY (this window or the one stored with setx).
#>
param(
    [ValidateSet('none', 'patch', 'minor', 'major')][string]$Bump = 'none',
    [string]$Version = '',
    [switch]$SkipScan,
    [switch]$KeepQaBuild,
    [switch]$AllowFlags   # accept VirusTotal detections (not for public releases)
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$runtime = Join-Path $root 'recomp\third_party\WiiCompiled\runtime'
$buildDir = Join-Path $root 'recomp\build\RM3E01_00_runtime_link_probe'
$header = Join-Path $runtime 'include\prime_port_version.h'
$tools = Join-Path $root 'recomp\.tools\portable-tools'
$env:PATH = (Join-Path $tools 'llvm-mingw\bin') + ';' + (Join-Path $tools 'Ninja') + ';' + $env:PATH
$steps = @()
$started = Get-Date

function Step {
    param([string]$Name, [scriptblock]$Body)
    $t = [Diagnostics.Stopwatch]::StartNew()
    Write-Host "`n== $Name" -ForegroundColor Cyan
    try {
        $detail = & $Body
        $t.Stop()
        $script:steps += [pscustomobject]@{ step = $Name; status = 'OK'; seconds = [int]$t.Elapsed.TotalSeconds; detail = ($detail -join ' ') }
        Write-Host ("   OK  ({0}s) {1}" -f [int]$t.Elapsed.TotalSeconds, ($detail -join ' ')) -ForegroundColor Green
    } catch {
        $t.Stop()
        $script:steps += [pscustomobject]@{ step = $Name; status = 'FAIL'; seconds = [int]$t.Elapsed.TotalSeconds; detail = $_.Exception.Message }
        Write-Host ("   FAIL  {0}" -f $_.Exception.Message) -ForegroundColor Red
        Summary
        exit 1
    }
}
function Summary {
    Write-Host "`n==== Summary ====" -ForegroundColor Cyan
    $steps | Format-Table -AutoSize step, status, seconds, @{n='detail';e={ if ($_.detail.Length -gt 90) { $_.detail.Substring(0, 90) + '...' } else { $_.detail } }} | Out-String -Width 200 | Write-Host
    Write-Host ("total {0:mm\:ss}" -f ((Get-Date) - $started))
}
# PowerShell 5.1 turns a native command's stderr into terminating errors when $ErrorActionPreference
# is 'Stop', and cmake writes its status messages there. Native calls run with it relaxed and are
# judged by their exit code alone.
function Native([scriptblock]$Command) {
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { $output = & $Command 2>&1 | Out-String } finally { $ErrorActionPreference = $previous }
    return [pscustomobject]@{ Code = $LASTEXITCODE; Output = $output }
}
function Configure([string]$qa) {
    $r = Native { cmake -S $runtime -B $buildDir "-DMKW_PRIME_QA_TOOLS=$qa" }
    if ($r.Code -ne 0) {
        $errors = ($r.Output -split "`r?`n" | Where-Object { $_ -match 'CMake Error|error:' } | Select-Object -First 3) -join ' | '
        throw "cmake configure failed (MKW_PRIME_QA_TOOLS=$qa): $errors"
    }
}
function BuildGame {
    $r = Native { cmake --build $buildDir --target WiiCompiled --parallel 2 }
    if ($r.Code -ne 0) {
        $errors = ($r.Output -split "`r?`n" | Where-Object { $_ -match ' error:|FAILED' } | Select-Object -First 3) -join ' | '
        throw "build failed: $errors"
    }
}

# ---------------------------------------------------------------- 1. version
Step 'Version' {
    $text = Get-Content -LiteralPath $header -Raw
    if ($text -notmatch '#define PRIME_PORT_VERSION "([^"]+)"') { throw 'PRIME_PORT_VERSION not found' }
    $current = $Matches[1]
    $target = $current
    if ($Version) {
        if ($Version -notmatch '^\d+\.\d+\.\d+$') { throw "-Version must look like 0.1.4 (got '$Version')" }
        $target = $Version
    } elseif ($Bump -ne 'none') {
        $p = $current.Split('.') | ForEach-Object { [int]$_ }
        switch ($Bump) {
            'patch' { $p[2]++ }
            'minor' { $p[1]++; $p[2] = 0 }
            'major' { $p[0]++; $p[1] = 0; $p[2] = 0 }
        }
        $target = "$($p[0]).$($p[1]).$($p[2])"
    }
    if ($target -ne $current) {
        $p = $target.Split('.')
        $text = $text -replace '(#define PRIME_PORT_VERSION\s+")[^"]+(")', "`${1}$target`${2}"
        $text = $text -replace '(#define PRIME_PORT_VERSION_MAJOR\s+)\d+', "`${1}$($p[0])"
        $text = $text -replace '(#define PRIME_PORT_VERSION_MINOR\s+)\d+', "`${1}$($p[1])"
        $text = $text -replace '(#define PRIME_PORT_VERSION_PATCH\s+)\d+', "`${1}$($p[2])"
        Set-Content -LiteralPath $header -Value $text -Encoding UTF8 -NoNewline
    }
    $script:releaseVersion = $target
    if ($target -eq $current) { "$target (unchanged)" } else { "$current -> $target" }
}

# ---------------------------------------------------------------- 2. changelog
Step 'Changelog' {
    $changelog = Join-Path $root 'publish\CHANGELOG.md'
    $section = Select-String -Path $changelog -Pattern "^## v$([regex]::Escape($releaseVersion))\b" | Select-Object -First 1
    if (-not $section) {
        throw "publish\CHANGELOG.md has no '## v$releaseVersion' section. Add it (see the v0.1.2 entry for the format) and run again."
    }
    if (Select-String -Path $changelog -Pattern '^## Unreleased' -Quiet) {
        Write-Host '   note: an "## Unreleased" section is still there; move its entries into the version section if they belong to it.' -ForegroundColor Yellow
    }
    "section found: " + $section.Line.Trim()
}

# ---------------------------------------------------------------- 3. the game must not be running
Step 'Close the game' {
    $running = @(Get-Process | Where-Object { $_.ProcessName -like 'Corruption_3*' })
    if (-not $running) { return 'not running' }
    $running | Stop-Process -Force
    Start-Sleep 2
    "closed pid " + ($running.Id -join ', ')
}

# ---------------------------------------------------------------- 4. build without the QA tools
Step 'Build (QA tools off)' {
    Configure 'OFF'
    BuildGame
    $exe = Join-Path $buildDir 'Corruption_3 PC.exe'
    $fileVersion = (Get-Item -LiteralPath $exe).VersionInfo.FileVersion
    if ($fileVersion -ne $releaseVersion) { throw "the executable reports $fileVersion, expected $releaseVersion" }
    if (([IO.File]::ReadAllText($exe, [Text.Encoding]::GetEncoding(28591))).Contains('MKW_PRIME_QA_TOOLS_ENABLED_BUILD')) {
        throw 'the executable still carries the QA tools'
    }
    "v$fileVersion, no QA marker, {0:N0} MB" -f ((Get-Item -LiteralPath $exe).Length / 1MB)
}

# ---------------------------------------------------------------- 5. package
Step 'Package' {
    $r = Native { powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'package_release.ps1') }
    if ($r.Code -ne 0 -or $r.Output -notmatch 'sha256:\s+([0-9a-f]{64})') {
        throw ("packaging failed: " + (($r.Output -split "`r?`n" | Where-Object { $_ } | Select-Object -Last 2) -join ' | '))
    }
    $script:zipHash = $Matches[1]
    $script:zipPath = Join-Path $root "dist\Corruption_3-PC-v$releaseVersion-win64.zip"
    $script:stagePath = Join-Path $root "dist\Corruption_3-PC-v$releaseVersion-win64"
    "{0} ({1:N0} MB)" -f (Split-Path $zipPath -Leaf), ((Get-Item -LiteralPath $zipPath).Length / 1MB)
}

# ---------------------------------------------------------------- 6. verify what a player gets
Step 'Verify the package' {
    $extract = Join-Path $env:TEMP "c3release_$releaseVersion"
    if (Test-Path $extract) { Remove-Item $extract -Recurse -Force }
    Expand-Archive -LiteralPath $zipPath -DestinationPath $extract
    $exe = Join-Path $extract 'Corruption_3 PC.exe'
    $problems = @()
    if (-not (Test-Path $exe)) { $problems += 'no executable' }
    elseif ((Get-Item $exe).VersionInfo.FileVersion -ne $releaseVersion) { $problems += 'wrong version in the zip' }
    if (Test-Path (Join-Path $extract 'UserData')) { $problems += 'UserData leaked into the zip' }
    foreach ($needed in 'Config.default.toml', 'README.md', 'CHANGELOG.md', 'LICENSE', 'THIRD-PARTY-NOTICES.md', 'initial_pipeline_cache.db', 'webgpu_dawn.dll') {
        if (-not (Test-Path (Join-Path $extract $needed))) { $problems += "missing $needed" }
    }
    $hash = (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($hash -ne $zipHash) { $problems += 'sha256 mismatch' }
    $entries = @(Get-ChildItem $extract -Recurse -File).Count
    Remove-Item $extract -Recurse -Force
    if ($problems) { throw ($problems -join '; ') }
    "$entries files, sha256 $($zipHash.Substring(0,16))..."
}

# ---------------------------------------------------------------- 7. put the QA build back (before the scan, so a flagged scan never leaves the local build without them)
if ($KeepQaBuild) {
    $steps += [pscustomobject]@{ step = 'Restore QA build'; status = 'SKIPPED'; seconds = 0; detail = '-KeepQaBuild' }
} else {
    Step 'Restore QA build' {
        Configure 'ON'
        BuildGame
        'local build has the QA tools again (recording and replay)'
    }
}

# ---------------------------------------------------------------- 8. antivirus report (a detection stops the release)
if ($SkipScan) {
    $steps += [pscustomobject]@{ step = 'VirusTotal'; status = 'SKIPPED'; seconds = 0; detail = '-SkipScan' }
    Write-Host "`n== VirusTotal`n   SKIPPED (-SkipScan)" -ForegroundColor Yellow
} else {
    Step 'VirusTotal' {
        $report = "$zipPath.virustotal.md"
        & (Join-Path $PSScriptRoot 'scan_release_virustotal.ps1') -Path @($zipPath, (Join-Path $stagePath 'Corruption_3 PC.exe')) -ReportPath $report | Out-Null
        if (-not (Test-Path $report)) { throw 'no report produced' }
        $script:scanReport = $report
        $counts = @(Get-Content $report | Select-String '(\d+) of (\d+) engines flagged it' | ForEach-Object { [int]$_.Matches[0].Groups[1].Value })
        $flagged = $counts -join '/'
        # The user's rule: nothing is published with an antivirus alert. A flagged file stops the release
        # here; investigate the engine and fix the build (see docs/publicar_version.md) instead of uploading.
        if (($counts | Measure-Object -Sum).Sum -gt 0 -and -not $AllowFlags) {
            throw "VirusTotal flagged the release ($flagged): " + ((Get-Content $report | Select-String 'Flagged by') -join ' | ') + " - see $report"
        }
        "flagged $flagged, report: " + (Split-Path $report -Leaf)
    }
}

Summary
Write-Host "`nPackage ready, nothing uploaded." -ForegroundColor Green
Write-Host "  zip     $zipPath"
Write-Host "  sha256  $zipHash"
if ($scanReport) { Write-Host "  scan    $scanReport"; Write-Host "`n--- antivirus report ---"; Get-Content $scanReport | Write-Host }
Write-Host "`nTo publish (separate step): powershell -NoProfile -ExecutionPolicy Bypass -File recomp\tools\publish_release.ps1 -Draft -SkipPackage"
