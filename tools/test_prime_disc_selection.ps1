<#
 Disc image selection checks against the real executable (GitHub discussions #4 and #5).
 Each case runs the game with its own throwaway UserData profile (WII_COMPILED_QA_USER_DATA,
 test build only), so the player's Config.toml is never read or written. No file dialog is
 shown: CORRUPTION3_NO_DISC_DIALOG=1 turns a failed check into the fatal message, and
 WII_COMPILED_AUTOMATED_PROBE=1 logs that message instead of showing it.

   -Iso <path>   a valid RM3E01 image (required for the cases that boot)
   -Seconds <n>  how long a booting case may run before it is closed (default 25)

 Writes recomp/checkpoints/20261111_disc_selection.json. Exit code 1 if a case fails.
#>
param(
    [Parameter(Mandatory = $true)][string]$Iso,
    [int]$Seconds = 25,
    [string]$BuildDirectory = 'recomp/build/RM3E01_00_runtime_link_probe',
    # Optional other-region images: the check must name what it found.
    [string]$CorruptionPal = '',
    [string]$TrilogyPal = '',
    # Optional Trilogy USA image: the Corruption executable must hand it to the Trilogy menu.
    [string]$TrilogyUsa = ''
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$build = Join-Path $root $BuildDirectory
$exe = Join-Path $build 'Corruption_3 PC.exe'
$work = Join-Path ([IO.Path]::GetTempPath()) ('prime_disc_selection_' + [Guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force $work | Out-Null
$iso = (Resolve-Path -LiteralPath $Iso).Path
$results = @(); $failed = $false

function New-Profile([string]$name, [string]$configText) {
    $dir = Join-Path $work $name
    New-Item -ItemType Directory -Force $dir | Out-Null
    if ($null -ne $configText) {
        [IO.File]::WriteAllText((Join-Path $dir 'Config.toml'), $configText, [Text.UTF8Encoding]::new($false))
    }
    return $dir
}

function Invoke-Case([string]$name, [string]$configText, [string[]]$arguments, [hashtable]$environment, [string[]]$expect, [string[]]$reject, [bool]$boots, [string]$expectConfig) {
    $profile = New-Profile $name $configText
    $configBefore = if ($null -ne $configText) { $configText } else { '' }
    $saved = @{}
    $vars = @{ WII_COMPILED_QA_USER_DATA = $profile; WII_COMPILED_AUTOMATED_PROBE = '1'; CORRUPTION3_NO_DISC_DIALOG = '1'; CORRUPTION3_ISO = $null }
    foreach ($k in $environment.Keys) { $vars[$k] = $environment[$k] }
    foreach ($k in $vars.Keys) { $saved[$k] = [Environment]::GetEnvironmentVariable($k, 'Process'); [Environment]::SetEnvironmentVariable($k, $vars[$k], 'Process') }
    try {
        $start = @{ FilePath = $exe; PassThru = $true; WindowStyle = 'Minimized' }
        # PowerShell 5.1 joins ArgumentList with spaces and does not quote: quote here.
        if ($arguments.Count) { $start.ArgumentList = ($arguments | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }) -join ' ' }
        $process = Start-Process @start
    } finally {
        foreach ($k in $saved.Keys) { [Environment]::SetEnvironmentVariable($k, $saved[$k], 'Process') }
    }
    $exited = $process.WaitForExit($Seconds * 1000)
    if (-not $exited) { Stop-Process -Id $process.Id -Force; $process.WaitForExit(10000) | Out-Null }
    # A process started by this one (the Trilogy menu) opens its log folder only once its parent has
    # exited, and the log is written asynchronously: read again for up to 15 s while something is missing.
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    do {
        $logs = @(Get-ChildItem -LiteralPath (Join-Path $profile 'Logs') -Recurse -File -Filter 'console.log' -ErrorAction SilentlyContinue)
        $text = ($logs | ForEach-Object { Get-Content -LiteralPath $_.FullName -Raw }) -join "`n"
        $crash = @(Get-ChildItem -LiteralPath (Join-Path $profile 'Logs') -Recurse -File -Filter 'crash_*.txt' -ErrorAction SilentlyContinue)
        $text += "`n" + (($crash | ForEach-Object { Get-Content -LiteralPath $_.FullName -Raw }) -join "`n")
        $missing = @($expect | Where-Object { $text -notmatch [regex]::Escape($_) })
        if (-not $missing.Count) { break }
        Start-Sleep -Milliseconds 500
    } while ([DateTime]::UtcNow -lt $deadline)
    $problems = @()
    foreach ($e in $expect) { if ($text -notmatch [regex]::Escape($e)) { $problems += "missing: $e" } }
    foreach ($r in $reject) { if ($text -match [regex]::Escape($r)) { $problems += "unexpected: $r" } }
    if ($boots -and $exited) { $problems += "exited before $Seconds s (code $($process.ExitCode))" }
    if (-not $boots -and -not $exited) { $problems += 'still running: expected a fatal stop' }
    $configAfter = ''
    $configPath = Join-Path $profile 'Config.toml'
    if (Test-Path -LiteralPath $configPath) { $configAfter = Get-Content -LiteralPath $configPath -Raw }
    if ($null -ne $expectConfig -and $configAfter -notmatch [regex]::Escape($expectConfig)) { $problems += "Config.toml lacks: $expectConfig" }
    $usesIso = @($arguments | Where-Object { $_ -eq '--iso' -or $_ -like '--iso=*' }).Count -gt 0
    if ($usesIso -and $configAfter -ne $configBefore -and $configAfter -match 'iso_path') {
        $beforeIso = ([regex]::Match($configBefore, '(?m)^iso_path.*$')).Value.Trim()
        $afterIso = ([regex]::Match($configAfter, '(?m)^iso_path.*$')).Value.Trim()
        if ($beforeIso -ne $afterIso) { $problems += '--iso was written to Config.toml' }
    }
    $ok = $problems.Count -eq 0
    if (-not $ok) { $script:failed = $true }
    Write-Host ("{0,-28} {1}  {2}" -f $name, ($(if ($ok) { 'PASS' } else { 'FAIL' })), ($problems -join '; '))
    $script:results += [ordered]@{ case = $name; passed = $ok; exited = $exited; problems = $problems }
}

$isoToml = $iso.Replace('\', '\\')
$shell = '::{20d04fe0-3aea-1069-a2d8-08002b30309d}\\Metroid Prime 3 - Corruption (Europe) (En,Fr,De,Es,It)'
$notDisc = Join-Path $work 'not_a_disc.iso'
[IO.File]::WriteAllBytes($notDisc, [byte[]](1..4096 | ForEach-Object { 0x41 }))

# Discussion #4: the value Proton saved. Refused with the shell-location text; no game folder here.
Invoke-Case 'proton_shell_path' "[paths]`niso_path = `"$shell`"`n" @() @{} @('shell location', '[prime-iso-check] failed kind=1') @('[prime-iso-open]') $false $null
# --iso and CORRUPTION3_ISO boot without touching Config.toml.
Invoke-Case 'iso_argument' $null @('--iso', $iso) @{} @('[prime-iso-arg] --iso', '[prime-iso-open] id=RM3E01', 'source=--iso / CORRUPTION3_ISO') @('[prime-iso-check]') $true $null
Invoke-Case 'iso_argument_equals' $null @("--iso=$iso") @{} @('source=--iso / CORRUPTION3_ISO') @('[prime-iso-check]') $true $null
Invoke-Case 'iso_environment' $null @() @{ CORRUPTION3_ISO = "`"$iso`"" } @('source=--iso / CORRUPTION3_ISO') @('[prime-iso-check]') $true $null
# --iso wins over a broken saved value.
Invoke-Case 'iso_beats_shell_config' "[paths]`niso_path = `"$shell`"`n" @('--iso', $iso) @{} @('source=--iso / CORRUPTION3_ISO') @('[prime-iso-check]') $true $null
# A valid saved value still works as before.
Invoke-Case 'config_path' "[paths]`niso_path = `"$isoToml`"`n" @() @{} @('source=Config.toml [paths] iso_path') @('[prime-iso-check]') $true $null
# Distinct failures (discussion #5, item 3).
Invoke-Case 'iso_missing_file' $null @('--iso', (Join-Path $work 'moved.rvz')) @{} @('There is no file at this path', 'kind=1') @('[prime-iso-open]') $false $null
Invoke-Case 'iso_folder' $null @('--iso', $work) @{} @('is a folder', 'kind=1') @('[prime-iso-open]') $false $null
Invoke-Case 'iso_not_a_disc' $null @('--iso', $notDisc) @{} @('could not be read as a Wii disc image', 'kind=2') @('[prime-iso-open]') $false $null
Invoke-Case 'no_image' $null @() @{} @('No Metroid Prime 3: Corruption disc image was found', 'kind=0', '--iso') @('[prime-iso-open]') $false $null
if ($CorruptionPal) {
    Invoke-Case 'corruption_pal' $null @('--iso', (Resolve-Path -LiteralPath $CorruptionPal).Path) @{} @('Found RM3P01 - Metroid Prime 3: Corruption (Europe)', 'Other regions', 'kind=3') @('[prime-iso-open]') $false $null
}
if ($TrilogyPal) {
    Invoke-Case 'trilogy_pal' $null @('--iso', (Resolve-Path -LiteralPath $TrilogyPal).Path) @{} @('Found R3MP01 - Metroid Prime Trilogy (Europe)', 'kind=3') @('[prime-iso-open]', 'prime-trilogy') $false $null
}
if ($TrilogyUsa) {
    Invoke-Case 'trilogy_usa_handoff' $null @('--iso', (Resolve-Path -LiteralPath $TrilogyUsa).Path) @{} @('Corruption_3 Trilogy Menu.exe') @('[prime-iso-check]') $false $null
    # The menu it started inherits the throwaway profile (its console.log lands there,
    # which is the evidence: the exiting process may end before its own log line is written); close it.
    Get-Process -Name 'Corruption_3 Trilogy Menu', 'Corruption_3 Trilogy MP3' -ErrorAction SilentlyContinue | Stop-Process -Force
}
Invoke-Case 'unknown_argument' $null @('--skip-intro') @{} @('Unknown argument: --skip-intro') @('[prime-iso-open]') $false $null

Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
$report = [ordered]@{
    tested_utc = [DateTime]::UtcNow.ToString('o'); revision = 'RM3E01_00'; passed = -not $failed
    executable_sha256 = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToLowerInvariant()
    cases = $results
    not_covered = 'The file dialogs themselves (Vista IFileOpenDialog with SIGDN_FILESYSPATH, legacy fallback), saving a dialog pick after it opens, the F1 typed path and the Trilogy hand-off need a person at the screen. Wrong-region text is covered by mkw_prime_disc_path_tests (no PAL image here).'
}
[IO.File]::WriteAllText((Join-Path $root 'recomp/checkpoints/20261111_disc_selection.json'), ($report | ConvertTo-Json -Depth 5) + [Environment]::NewLine, [Text.UTF8Encoding]::new($false))
if ($failed) { Write-Host 'FAILED'; exit 1 } else { Write-Host 'ALL PASSED' }
