# Unattended A/B timing run: boots Corruption_3 PC.exe without input, lets the
# title reach its own scene and reports Aurora's 5 s present-rate windows from
# the session log. No buttons are pressed; the window is closed at the end.
# Scenes reached this way depend on the save state and idle camera, so compare
# runs only at equal elapsed times and only with the same save.
param(
    [int]$Seconds = 100,
    [switch]$GpuThread,
    [switch]$OverlapEncode,
    [switch]$LazyEndDrain,
    [switch]$PresentOnCopy,
    [string]$Label = 'run',
    [int]$ProfileAt = 0,
    [double]$ProfileBelowFps = 0
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$dir = Join-Path $root 'recomp/build/RM3E01_00_runtime_link_probe'
if (@(Get-Process -Name 'WiiCompiled' -ErrorAction SilentlyContinue).Count) { throw 'Close WiiCompiled first.' }
$vars = @{ WII_COMPILED_PRIME_KEYBOARD_MOUSE = '1'; WII_COMPILED_PRIME_PERFORMANCE_TRACE = '1' }
if ($GpuThread) { $vars.WII_COMPILED_PRIME_GPU_THREAD = '1' }
if ($OverlapEncode) { $vars.WII_COMPILED_AURORA_OVERLAP_ENCODE = '1' }
if ($LazyEndDrain) { $vars.WII_COMPILED_AURORA_LAZY_END_DRAIN = '1' }
if ($PresentOnCopy) { $vars.WII_COMPILED_PRIME_PRESENT_ON_COPY = '1' }
if ($ProfileAt -gt 0) { $vars.WII_COMPILED_STACK_PROFILE_AT = "$ProfileAt" }
if ($ProfileBelowFps -gt 0) { $vars.WII_COMPILED_STACK_PROFILE_BELOW_FPS = "$ProfileBelowFps" }
$saved = @{}
foreach ($k in $vars.Keys) { $saved[$k] = [Environment]::GetEnvironmentVariable($k, 'Process'); [Environment]::SetEnvironmentVariable($k, $vars[$k], 'Process') }
try { $p = Start-Process -FilePath (Join-Path $dir 'Corruption_3 PC.exe') -WorkingDirectory $dir -PassThru }
finally { foreach ($k in $vars.Keys) { [Environment]::SetEnvironmentVariable($k, $saved[$k], 'Process') } }
$started = Get-Date
Start-Sleep -Seconds $Seconds
$p.Refresh()
$alive = -not $p.HasExited
if ($alive) { Stop-Process -Id $p.Id }
$log = Get-ChildItem (Join-Path $dir 'UserData/Logs') -Directory | Where-Object { $_.Name -like "*pid$($p.Id)" } | Select-Object -First 1
$rows = @()
foreach ($line in (Select-String -Path (Join-Path $log.FullName '*.log') -Pattern '\[prime-performance\] presents=(\d+) window_samples=\d+ fps=([\d.]+) effective_fps=[\d.]+ average_ms=([\d.]+) p95_ms=([\d.]+)')) {
    $m = $line.Matches[0]
    $rows += [pscustomobject]@{ presents = [int]$m.Groups[1].Value; fps = [double]$m.Groups[2].Value; avg_ms = [double]$m.Groups[3].Value; p95_ms = [double]$m.Groups[4].Value }
}
[pscustomobject]@{ label = $Label; pid = $p.Id; alive_at_end = $alive; log = $log.FullName; windows = $rows } | ConvertTo-Json -Depth 4
