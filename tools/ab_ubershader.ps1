# A/B of the hybrid ubershader on materials that compile during play.
#   ab_ubershader.ps1 -Uber 0|1 [-SkipOneIn 8] [-PlaySeconds 60]
# Leaves one specialised recipe in N out of the boot prewarm and makes the Dawn cache miss after boot,
# loads the save in slot 1, walks around, and reports FPS windows, long frames and ubershader use.
param([int]$Uber = 1, [int]$SkipOneIn = 8, [int]$PlaySeconds = 60, [string]$Shots = '')
$ErrorActionPreference = 'Stop'
$d = Join-Path $PSScriptRoot 'drive_prime.ps1'
function D { param([Parameter(ValueFromRemainingArguments = $true)]$a) & powershell -NoProfile -ExecutionPolicy Bypass -File $d @a }
D start -Env "WII_COMPILED_QA_TAP_INTRO=1;WII_COMPILED_UBERSHADER=$Uber;WII_COMPILED_PREWARM_SKIP_ONE_IN=$SkipOneIn;WII_COMPILED_DAWN_CACHE_COLD_AFTER_BOOT=1;WII_COMPILED_PRIME_PERFORMANCE_TRACE=1;WII_COMPILED_UBER_STATS=1" | Out-Null
Start-Sleep 40; D key Enter | Out-Null
Start-Sleep 8; D click 900 365 | Out-Null
Start-Sleep 7; D click 500 355 | Out-Null
Start-Sleep 45
$end = (Get-Date).AddSeconds($PlaySeconds); $i = 0
while ((Get-Date) -lt $end) {
    D key W 1800 | Out-Null
    D move (700 + ($i % 3) * 250) 500 | Out-Null
    if ($Shots -and $i -eq 2) { D shot "$Shots" | Out-Null }
    $i++
}
$log = (D log | Select-Object -Last 1)
D stop | Out-Null
$text = Get-Content (Join-Path $log 'console.log')
$fps = $text | Select-String '\[prime-performance\] presents=\d+ window_samples=\d+ fps=([\d.]+).*p95_ms=([\d.]+)' | ForEach-Object { [double]$_.Matches[0].Groups[1].Value }
$compiles = @($text | Select-String '\[pipeline-compile\] ms=([\d.]+)' | ForEach-Object { [double]$_.Matches[0].Groups[1].Value })
$uberLines = @($text | Select-String '\[uber\] draws encoded with an ubershader so far: (\d+)' | ForEach-Object { [int64]$_.Matches[0].Groups[1].Value })
$lat = @($text | Select-String 'input_to_copy_worst_ms=([\d.]+)' | ForEach-Object { [double]$_.Matches[0].Groups[1].Value })
$play = $fps | Select-Object -Skip 18
[ordered]@{
    uber = $Uber; log = $log
    fps_windows_in_play = ($play | ForEach-Object { [Math]::Round($_, 1) }) -join ' '
    min_fps = [Math]::Round(($play | Measure-Object -Minimum).Minimum, 1)
    mean_fps = [Math]::Round(($play | Measure-Object -Average).Average, 2)
    compiles_ge20ms = $compiles.Count
    compile_ms_max = if ($compiles) { ($compiles | Measure-Object -Maximum).Maximum } else { 0 }
    worst_frame_ms_per_window = ($lat | ForEach-Object { [Math]::Round($_) }) -join ' '
    uber_draws_last_report = if ($uberLines) { $uberLines[-1] } else { 0 }
} | ConvertTo-Json
