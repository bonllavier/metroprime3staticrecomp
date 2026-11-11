# Records play sessions for automated replays (recomp/qa/README.md). The game keeps running between
# recordings: start and stop are messages to the running game.
#   qa_record.ps1 -Launch                 start the game ready to record (nothing recorded yet)
#   qa_record.ps1 -Name hangar [-Notes..] start recording (at the title or the menus); launches the game if needed
#   qa_record.ps1 -Stop                   stop the current recording; the game keeps running
#   qa_record.ps1 -Status                 what the game reports (idle / recording / stopped / refused)
param([string]$Name = '', [string]$Notes = '', [switch]$Launch, [switch]$Stop, [switch]$Status)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$build = Join-Path $root 'recomp\build\RM3E01_00_runtime_link_probe'
$userData = Join-Path $build 'UserData'
$control = Join-Path $PSScriptRoot 'control'
New-Item -ItemType Directory -Force -Path $control | Out-Null
$game = Get-Process | Where-Object { $_.ProcessName -like 'Corruption_3*' } | Select-Object -First 1

function Launch-Game {
    $exe = Join-Path $build 'Corruption_3 PC.exe'
    if (-not ([IO.File]::ReadAllText($exe, [Text.Encoding]::GetEncoding(28591))).Contains('MKW_PRIME_QA_TOOLS_ENABLED_BUILD')) {
        throw 'The executable was built without MKW_PRIME_QA_TOOLS; configure the build with -DMKW_PRIME_QA_TOOLS=ON.'
    }
    Remove-Item (Join-Path $control '*.txt') -ErrorAction SilentlyContinue
    $env:WII_COMPILED_QA_CONTROL_DIR = $control
    $env:WII_COMPILED_PRIME_KEYBOARD_MOUSE = '1'
    $p = Start-Process -FilePath $exe -WorkingDirectory $build -PassThru
    Write-Host "Game started (pid $($p.Id)), ready to record. Wait for the title screen."
    return $p
}

if ($Status) {
    $f = Join-Path $control 'status.txt'
    if (Test-Path $f) { Get-Content $f } else { 'no status (game not started with qa_record.ps1 -Launch / -Name)' }
    return
}
if ($Stop) {
    if (-not $game) { throw 'The game is not running.' }
    Set-Content -LiteralPath (Join-Path $control 'stop.txt') -Value 'stop' -Encoding ASCII
    Start-Sleep 2
    Get-Content (Join-Path $control 'status.txt') -ErrorAction SilentlyContinue
    return
}
if ($Launch -and -not $Name) {
    if ($game) { throw 'The game is already running.' }
    Launch-Game | Out-Null
    return
}
if (-not $Name) { throw 'Give -Name <recording>, -Stop, -Status or -Launch.' }
if ($Name -notmatch '^[A-Za-z0-9_-]+$') { throw 'Name: letters, digits, _ and - only' }
$rec = Join-Path $PSScriptRoot "recordings\$Name"
if (Test-Path $rec) { throw "Recording '$Name' exists; pick another name or delete it." }
if (-not $game) {
    $game = Launch-Game
    Write-Host 'When the title screen is up, run this command again with the same -Name to start recording.'
    return
}
# Starting state for replays: the saved game on disk and the configuration, as they are right now.
New-Item -ItemType Directory -Path $rec | Out-Null
Copy-Item -LiteralPath (Join-Path $userData 'NAND') -Destination (Join-Path $rec 'NAND') -Recurse
Copy-Item -LiteralPath (Join-Path $userData 'Config.toml') -Destination (Join-Path $rec 'Config.toml')
[ordered]@{ name = $Name; recorded_utc = [DateTime]::UtcNow.ToString('o'); notes = $Notes
    exe_sha256 = (Get-FileHash -LiteralPath (Join-Path $build 'Corruption_3 PC.exe') -Algorithm SHA256).Hash.ToLowerInvariant() } |
    ConvertTo-Json | Set-Content -LiteralPath (Join-Path $rec 'meta.json') -Encoding UTF8
Set-Content -LiteralPath (Join-Path $control 'start.txt') -Value (Join-Path $rec 'inputs.txt') -Encoding ASCII
Start-Sleep 2
$reported = Get-Content (Join-Path $control 'status.txt') -ErrorAction SilentlyContinue
Write-Host $reported
if ($reported -like 'refused*') { Remove-Item -LiteralPath $rec -Recurse -Force }
else { Write-Host "Play the tour, then: qa_record.ps1 -Stop   (the game keeps running)" }
