# Self-check of record/replay without a human: launches the game ready to record, starts a recording at
# the title, drives a short menu + walk tour with synthetic input, stops, closes, replays it and compares
# a screenshot taken at the same point of both runs. Private QA tool (recomp/qa/README.md).
param([string]$Name = 'selfcheck')
$ErrorActionPreference = 'Stop'
$qa = $PSScriptRoot
$tools = Join-Path $qa '..\tools'
function Q { param([Parameter(ValueFromRemainingArguments = $true)]$a) & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $qa 'qa_record.ps1') @a }
function D { param([Parameter(ValueFromRemainingArguments = $true)]$a) & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $tools 'drive_prime.ps1') @a }
$rec = Join-Path $qa "recordings\$Name"
if (Test-Path $rec) { Remove-Item -LiteralPath $rec -Recurse -Force }
Q -Launch | Out-Null
$game = $null
for ($i = 0; $i -lt 30 -and -not $game; $i++) { Start-Sleep 1; $game = Get-Process | Where-Object { $_.ProcessName -like 'Corruption_3*' } | Select-Object -First 1 }
if (-not $game) { throw 'game did not start' }
Set-Content -LiteralPath (Join-Path $tools '..\.tools\drive_prime.pid') -Value $game.Id   # let drive_prime.ps1 drive this instance
Start-Sleep 55
"-- start recording at the title"
Q -Name $Name -Notes 'automatic self-check'
Start-Sleep 3
D key Enter | Out-Null; Start-Sleep 9
D click 900 365 | Out-Null; Start-Sleep 7
D click 500 355 | Out-Null; Start-Sleep 45
D key W 2000 | Out-Null; Start-Sleep 1
D move 1100 500 | Out-Null; Start-Sleep 1
D key W 1500 | Out-Null; Start-Sleep 2
D shot (Join-Path $rec 'record_end.png') | Out-Null
"-- stop recording"
Q -Stop
Start-Sleep 2
D stop | Out-Null
Start-Sleep 4
$lines = (Get-Content (Join-Path $rec 'inputs.txt') | Measure-Object -Line).Lines
"recorded lines: $lines"
"-- replay"
$report = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $qa 'qa_replay.ps1') -Name $Name -Label selfcheck -ShotEvery 10
$report
$run = ($report | ConvertFrom-Json).run_dir
$last = Join-Path $run 'shot_end.png'
if (Test-Path $last) {
    "-- compare the recording's last frame with the replay's end frame"
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $tools 'compare_images.ps1') (Join-Path $rec 'record_end.png') $last (Join-Path $run 'diff_end.png')
} else { "no end screenshot" }
