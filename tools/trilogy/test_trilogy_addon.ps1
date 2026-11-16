<#
 Tests the Metroid Prime Trilogy add-on as a player meets it (decision of 16/11/2026; recomp/docs/publicacion_github.md).
   test_trilogy_addon.ps1 -Image "<Metroid Prime Trilogy (USA).rvz>" [-Work recomp\titles\addon_test] [-KeepFiles]

 Uses the local build (QA tools allowed): package_release.ps1 -LayoutTest writes both zips into <Work>\dist, then
 a fresh player folder <Work>\player gets them unzipped step by step:
   1. archives      main zip without the Trilogy executables, add-on zip with exactly its five files
   2. missing       main zip only, Trilogy disc  -> "[prime-trilogy] add-on missing" naming the add-on archive
   3. incomplete    add-on without its MP3 exe   -> the message names "Corruption_3 Trilogy MP3.exe"
   4. installed     full add-on                  -> "[prime-trilogy] started the Trilogy menu", the menu runs
   5. mismatch      menu with another base version -> exits 1 with "[trilogy-exec] version mismatch"
   6. same version  menu with this base version  -> keeps running
 Runs without dialogs (CORRUPTION3_NO_DISC_DIALOG=1, WII_COMPILED_AUTOMATED_PROBE=1). Only processes started from
 the player folder are closed. The player's own UserData is never touched. Close the game first.
#>
param(
    [Parameter(Mandatory = $true)][string]$Image,
    [string]$Work = 'recomp\titles\addon_test',
    [switch]$KeepFiles
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$work = Join-Path $root $Work
$dist = Join-Path $work 'dist'
$player = Join-Path $work 'player'
$header = Get-Content -LiteralPath (Join-Path $root 'recomp\third_party\WiiCompiled\runtime\include\prime_port_version.h') -Raw
if ($header -notmatch '#define PRIME_PORT_VERSION "([^"]+)"') { throw 'PRIME_PORT_VERSION not found' }
$version = $Matches[1]
$mainZip = Join-Path $dist "Corruption_3-PC-v$version-win64.zip"
$addonZip = Join-Path $dist "Corruption_3-PC-v$version-Trilogy-addon-win64.zip"
if (-not (Test-Path -LiteralPath $Image)) { throw "Disc image not found: $Image" }
if (@(Get-Process | Where-Object { $_.ProcessName -like 'Corruption_3*' }).Count) { throw 'Close the game first.' }

$results = @()
function Result([string]$Test, [bool]$Ok, [string]$Detail) {
    $script:results += [pscustomobject]@{ test = $Test; result = $(if ($Ok) { 'PASS' } else { 'FAIL' }); detail = $Detail }
    Write-Host ("{0,-13} {1}  {2}" -f $Test, $(if ($Ok) { 'PASS' } else { 'FAIL' }), $Detail) -ForegroundColor $(if ($Ok) { 'Green' } else { 'Red' })
}
function StopPlayerProcesses {
    Get-Process | Where-Object { $_.ProcessName -like 'Corruption_3*' -and $_.Path -and $_.Path.StartsWith($player, [StringComparison]::OrdinalIgnoreCase) } |
        ForEach-Object { Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue }
    Start-Sleep -Milliseconds 800
}
function PlayerLogs { Get-ChildItem -LiteralPath (Join-Path $player 'UserData\Logs') -Recurse -Filter console.log -ErrorAction SilentlyContinue }
# Starts Corruption_3 PC.exe from the player folder with the image and waits for `pattern` in a log written after the
# start (or for the process to end); returns the matching text, or $null.
function RunMain([string]$Pattern, [int]$Seconds = 120) {
    $started = Get-Date
    $env:CORRUPTION3_NO_DISC_DIALOG = '1'; $env:WII_COMPILED_AUTOMATED_PROBE = '1'
    try { $process = Start-Process -FilePath (Join-Path $player 'Corruption_3 PC.exe') -ArgumentList @('--iso', "`"$Image`"") -WorkingDirectory $player -PassThru }
    finally { Remove-Item Env:CORRUPTION3_NO_DISC_DIALOG, Env:WII_COMPILED_AUTOMATED_PROBE -ErrorAction SilentlyContinue }
    $deadline = $started.AddSeconds($Seconds)
    $found = $null
    while ((Get-Date) -lt $deadline -and -not $found) {
        Start-Sleep -Seconds 2
        foreach ($log in @(PlayerLogs | Where-Object { $_.LastWriteTime -ge $started.AddSeconds(-2) })) {
            $hit = Select-String -LiteralPath $log.FullName -Pattern $Pattern -SimpleMatch -ErrorAction SilentlyContinue | Select-Object -First 1
            if ($hit) { $found = $hit.Line; break }
        }
        if (-not $found -and $process.HasExited -and ((Get-Date) - $process.ExitTime).TotalSeconds -gt 4) { break }
    }
    return $found
}
function LogsSince([datetime]$Since) {
    (@(PlayerLogs | Where-Object { $_.LastWriteTime -ge $Since }) | ForEach-Object { Get-Content -LiteralPath $_.FullName -Raw }) -join "`n"
}

try {
    # ---- 1. archives
    if (Test-Path -LiteralPath $work) { Remove-Item -LiteralPath $work -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $dist, $player | Out-Null
    $relative = $dist.Substring($root.Length).TrimStart('\')
    $output = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'recomp\tools\package_release.ps1') -OutputDirectory $relative -LayoutTest 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $mainZip) -or -not (Test-Path $addonZip)) { throw "package_release.ps1 -LayoutTest failed:`n$output" }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $mainEntries = [IO.Compression.ZipFile]::OpenRead($mainZip); $mainNames = @($mainEntries.Entries | ForEach-Object FullName); $mainEntries.Dispose()
    $addonEntries = [IO.Compression.ZipFile]::OpenRead($addonZip); $addonNames = @($addonEntries.Entries | ForEach-Object FullName | Sort-Object); $addonEntries.Dispose()
    $mainOk = ($mainNames -contains 'Corruption_3 PC.exe') -and -not ($mainNames | Where-Object { $_ -like 'Corruption_3 Trilogy*' })
    $addonOk = -not (Compare-Object $addonNames @('Corruption_3 Trilogy MP3.exe', 'Corruption_3 Trilogy Menu.exe', 'LICENSE', 'README - Trilogy add-on.txt', 'THIRD-PARTY-NOTICES.md'))
    $sizes = '{0:N1} MB + {1:N1} MB' -f ((Get-Item $mainZip).Length / 1MB), ((Get-Item $addonZip).Length / 1MB)
    Result 'archives' ($mainOk -and $addonOk) "main $($mainNames.Count) entries, add-on: $($addonNames -join ', ') ($sizes)"

    # ---- 2. main zip only
    Expand-Archive -LiteralPath $mainZip -DestinationPath $player
    $line = RunMain 'add-on missing'
    StopPlayerProcesses
    $text = LogsSince (Get-Date).AddMinutes(-10)
    Result 'missing' ([bool]$line -and $text.Contains("Corruption_3-PC-v$version-Trilogy-addon-win64.zip")) $(if ($line) { $line.Trim() } else { 'no add-on message in the log' })

    # ---- 3. add-on without its MP3 executable
    Expand-Archive -LiteralPath $addonZip -DestinationPath $player -Force
    $mp3 = Join-Path $player 'Corruption_3 Trilogy MP3.exe'
    $aside = "$mp3.aside"
    Move-Item -LiteralPath $mp3 -Destination $aside -Force
    $line = RunMain 'add-on missing'
    StopPlayerProcesses
    Result 'incomplete' ([bool]$line -and $line.Contains('Corruption_3 Trilogy MP3.exe') -and -not $line.Contains('Menu.exe')) $(if ($line) { $line.Trim() } else { 'no add-on message in the log' })
    Move-Item -LiteralPath $aside -Destination $mp3 -Force

    # ---- 4. full add-on: the Trilogy menu starts
    $since = Get-Date
    $line = RunMain 'started the Trilogy menu' 180
    $menuRunning = $false
    for ($i = 0; $i -lt 30 -and -not $menuRunning; ++$i) {
        Start-Sleep -Seconds 1
        $menuRunning = [bool](Get-Process | Where-Object { $_.ProcessName -eq 'Corruption_3 Trilogy Menu' -and $_.Path -and $_.Path.StartsWith($player, [StringComparison]::OrdinalIgnoreCase) })
    }
    Start-Sleep -Seconds 5
    $mismatchSeen = (LogsSince $since).Contains('version mismatch')
    StopPlayerProcesses
    # Corruption_3 PC ends right after starting the menu, so its last log line may not reach the file: the menu's own
    # log ("[trilogy-exec] saved region", written when its game code starts) is proof too.
    if (-not $line) { $line = ((LogsSince $since) -split "`n" | Where-Object { $_ -match 'started the Trilogy menu|\[trilogy-exec\] saved region' } | Select-Object -First 1) }
    Result 'installed' ([bool]$line -and $menuRunning -and -not $mismatchSeen) "$(if ($line) { $line.Trim() } else { 'menu not started' }); menu process: $menuRunning"

    # ---- 5./6. the menu under another version of Corruption_3 PC, then under this one
    $menu = Join-Path $player 'Corruption_3 Trilogy Menu.exe'
    foreach ($case in @(@{ Name = 'mismatch'; Base = '0.0.1' }, @{ Name = 'same version'; Base = $version })) {
        $stderr = Join-Path $work "stderr_$($case.Base).txt"
        # As Corruption_3 PC starts it: its version, and the image in CORRUPTION3_ISO (which --iso exports).
        $env:CORRUPTION3_BASE_VERSION = $case.Base; $env:WII_COMPILED_AUTOMATED_PROBE = '1'; $env:CORRUPTION3_NO_DISC_DIALOG = '1'; $env:CORRUPTION3_ISO = $Image
        # Reading .Handle right away keeps the exit code readable after the process ends (PowerShell 5.1).
        try { $process = Start-Process -FilePath $menu -WorkingDirectory $player -PassThru -RedirectStandardError $stderr; $null = $process.Handle }
        finally { Remove-Item Env:CORRUPTION3_BASE_VERSION, Env:WII_COMPILED_AUTOMATED_PROBE, Env:CORRUPTION3_NO_DISC_DIALOG, Env:CORRUPTION3_ISO -ErrorAction SilentlyContinue }
        $exited = $process.WaitForExit(15000)
        $err = if (Test-Path $stderr) { Get-Content -LiteralPath $stderr -Raw } else { '' }
        $mismatch = $err -match 'version mismatch'
        if ($case.Name -eq 'mismatch') {
            Result 'mismatch' ($exited -and $process.ExitCode -eq 1 -and $mismatch) "exited=$exited code=$(if ($exited) { $process.ExitCode } else { '-' }) stderr: $(($err -split "`n" | Select-String 'mismatch' | Select-Object -First 1))"
        } else {
            $why = if ($exited) { " (exit code $($process.ExitCode); stderr: $((($err -split "`n") | Where-Object { $_.Trim() } | Select-Object -Last 2) -join ' | '))" } else { '' }
            Result 'same version' ((-not $exited) -and -not $mismatch) "still running after 15 s: $(-not $exited)$why"
        }
        StopPlayerProcesses
    }
} finally {
    StopPlayerProcesses
    if (-not $KeepFiles -and (Test-Path -LiteralPath $work)) { Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue }
}
$results | Format-Table -AutoSize | Out-String -Width 220 | Write-Host
if ($results | Where-Object result -eq 'FAIL') { exit 1 }
