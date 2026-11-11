# Replays a recorded session unattended and reports performance (recomp/qa/README.md).
#   qa_replay.ps1 -Name hangar_walk [-Label test1] [-Env "VAR=1;VAR2=0"] [-ShotEvery 30] [-TimeoutMinutes 30]
# The replay runs on a private profile under recomp/qa/profile (the recording's save and settings, the
# shader caches shared with your profile through a junction), so your own save is never touched.
# -PrivateCache leaves the real shader caches out: for settings whose pipelines must not reach the shipped
# seed (prime_pipeline_seed.py merge reads the real cache and does not filter by MSAA level).
param([Parameter(Mandatory = $true)][string]$Name, [string]$Label = 'run', [string]$Env = '', [int]$ShotEvery = 30, [int]$TimeoutMinutes = 30, [switch]$PrivateCache)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$build = Join-Path $root 'recomp\build\RM3E01_00_runtime_link_probe'
$userData = Join-Path $build 'UserData'
$rec = Join-Path $PSScriptRoot "recordings\$Name"
if (-not (Test-Path (Join-Path $rec 'inputs.txt'))) { throw "No recording '$Name'" }
if (@(Get-Process | Where-Object { $_.ProcessName -like 'Corruption_3*' }).Count) { throw 'Close the game first (the replay shares the shader cache).' }
# Private profile: fresh copy of the recording's starting state every run.
$profile = Join-Path $PSScriptRoot 'profile\UserData'
if (Test-Path $profile) {
    $cache = Join-Path $profile 'Cache'
    # A junction is removed as a link only; a -PrivateCache folder goes with the profile below.
    if ((Test-Path $cache) -and ((Get-Item $cache -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) { [IO.Directory]::Delete($cache) }
    Remove-Item -LiteralPath $profile -Recurse -Force
}
New-Item -ItemType Directory -Path $profile | Out-Null
Copy-Item -LiteralPath (Join-Path $rec 'NAND') -Destination (Join-Path $profile 'NAND') -Recurse
Copy-Item -LiteralPath (Join-Path $rec 'Config.toml') -Destination (Join-Path $profile 'Config.toml')
# The disc image is wherever the player keeps it now, not where it was when the tour was recorded.
$realIso = Get-Content (Join-Path $userData 'Config.toml') -ErrorAction SilentlyContinue | Where-Object { $_ -match '^\s*iso_path\s*=' } | Select-Object -First 1
if ($realIso) {
    $cfg = Join-Path $profile 'Config.toml'
    $lines = @(Get-Content $cfg | ForEach-Object { if ($_ -match '^\s*iso_path\s*=') { $realIso } else { $_ } })
    Set-Content -LiteralPath $cfg -Value $lines -Encoding UTF8
}
if (-not $PrivateCache) { New-Item -ItemType Junction -Path (Join-Path $profile 'Cache') -Target (Join-Path $userData 'Cache') | Out-Null }
$runDir = Join-Path $PSScriptRoot "runs\$Name\$((Get-Date).ToString('yyyyMMdd_HHmmss'))_$Label"
New-Item -ItemType Directory -Path $runDir | Out-Null

$saved = @{}
$vars = @{ WII_COMPILED_QA_USER_DATA = $profile; WII_COMPILED_QA_REPLAY = (Join-Path $rec 'inputs.txt'); WII_COMPILED_QA_REPLAY_EXIT = '10'; WII_COMPILED_QA_TAP_INTRO = '1'
           WII_COMPILED_PRIME_KEYBOARD_MOUSE = '1'; WII_COMPILED_PRIME_PERFORMANCE_TRACE = '1' }
foreach ($pair in ($Env -split ';' | Where-Object { $_ })) { $k, $v = $pair -split '=', 2; $vars[$k] = $v }
foreach ($k in $vars.Keys) { $saved[$k] = [Environment]::GetEnvironmentVariable($k, 'Process'); [Environment]::SetEnvironmentVariable($k, $vars[$k], 'Process') }
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class QaWin { [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags); }
"@
function Shot($proc, $path) {
    try {
        # A screen copy of the window rectangle: the window has to be in front.
        # Windows refuses the foreground to a background script, but a topmost window needs no focus
        # (HWND_TOPMOST = -1; SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW = 0x43). Unattended runs only.
        [void][QaWin]::ShowWindow($proc.MainWindowHandle, 9)
        [void][QaWin]::SetWindowPos($proc.MainWindowHandle, [IntPtr](-1), 0, 0, 0, 0, 0x43)
        Start-Sleep -Milliseconds 400
        $r = New-Object QaWin+RECT; [void][QaWin]::GetClientRect($proc.MainWindowHandle, [ref]$r)
        $pt = New-Object QaWin+POINT; [void][QaWin]::ClientToScreen($proc.MainWindowHandle, [ref]$pt)
        if ($r.R -le 0) { return }
        $bmp = New-Object System.Drawing.Bitmap $r.R, $r.B; $g = [System.Drawing.Graphics]::FromImage($bmp)
        $g.CopyFromScreen($pt.X, $pt.Y, 0, 0, $bmp.Size); $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png); $g.Dispose(); $bmp.Dispose()
    } catch {}
}
try {
    $p = Start-Process -FilePath (Join-Path $build 'Corruption_3 PC.exe') -WorkingDirectory $build -PassThru
    $start = Get-Date; $nextShot = $start.AddSeconds($ShotEvery); $n = 0
    $endShot = $false; $liveLog = $null
    while (-not $p.HasExited -and ((Get-Date) - $start).TotalMinutes -lt $TimeoutMinutes) {
        Start-Sleep -Milliseconds 500; $p.Refresh()
        if ((Get-Date) -ge $nextShot) { Shot $p (Join-Path $runDir ("shot_{0:D4}s.png" -f [int]((Get-Date) - $start).TotalSeconds)); $nextShot = (Get-Date).AddSeconds($ShotEvery); $n++ }
        if (-not $endShot) {
            # The replay module quits 3 s after the last recorded frame: grab that frame for comparisons.
            if (-not $liveLog) { $liveLog = Get-ChildItem (Join-Path $profile 'Logs') -Directory -ErrorAction SilentlyContinue | Where-Object { $_.Name -like "*pid$($p.Id)" } | Select-Object -First 1 }
            # Only the tail: re-reading a multi-megabyte log every half second missed the exit window.
            if ($liveLog -and ((Get-Content (Join-Path $liveLog.FullName 'console.log') -Tail 40 -ErrorAction SilentlyContinue) -match 'qa-replay\] finished')) {
                Shot $p (Join-Path $runDir 'shot_end.png'); $endShot = $true
            }
        }
    }
    $timedOut = -not $p.HasExited
    if ($timedOut) { Stop-Process -Id $p.Id -Force }
} finally { foreach ($k in $saved.Keys) { [Environment]::SetEnvironmentVariable($k, $saved[$k], 'Process') } }
Start-Sleep 2
$log = Get-ChildItem (Join-Path $profile 'Logs') -Directory | Where-Object { $_.Name -like "*pid$($p.Id)" } | Sort-Object LastWriteTime -Descending | Select-Object -First 1
$text = Get-Content (Join-Path $log.FullName 'console.log')
Copy-Item (Join-Path $log.FullName 'console.log') (Join-Path $runDir 'console.log')
$fps = @($text | Select-String '\[prime-performance\] presents=\d+ window_samples=\d+ fps=([\d.]+)' | ForEach-Object { [double]$_.Matches[0].Groups[1].Value } | Select-Object -Skip 1)
$worst = @($text | Select-String 'input_to_copy_worst_ms=([\d.]+)' | ForEach-Object { [Math]::Round([double]$_.Matches[0].Groups[1].Value) })
$compiles = @($text | Select-String '\[pipeline-compile\] ms=([\d.]+)' | ForEach-Object { [double]$_.Matches[0].Groups[1].Value })
$finished = ($text | Select-String '\[qa-replay\] finished').Count -gt 0
$drift = $text | Select-String 'anchors_checked=(\d+) drift_events=(\d+) max_drift_m=([\d.]+) corrections=(\d+)' | Select-Object -Last 1
$report = [ordered]@{
    recording = $Name; label = $Label; env = $Env; run_dir = $runDir
    replay_finished = $finished; timed_out = $timedOut; screenshots = $n
    fps_windows = ($fps | ForEach-Object { [Math]::Round($_, 1) }) -join ' '
    mean_fps = if ($fps) { [Math]::Round(($fps | Measure-Object -Average).Average, 2) } else { 0 }
    min_fps = if ($fps) { [Math]::Round(($fps | Measure-Object -Minimum).Minimum, 1) } else { 0 }
    windows_below_59 = @($fps | Where-Object { $_ -lt 59 }).Count
    worst_frame_ms = if ($worst) { ($worst | Measure-Object -Maximum).Maximum } else { 0 }
    compiles_ge20ms = $compiles.Count
    anchors_checked = if ($drift) { [int]$drift.Matches[0].Groups[1].Value } else { 0 }
    drift_events = if ($drift) { [int]$drift.Matches[0].Groups[2].Value } else { 0 }
    max_drift_m = if ($drift) { [double]$drift.Matches[0].Groups[3].Value } else { 0 }
    corrections = if ($drift) { [int]$drift.Matches[0].Groups[4].Value } else { 0 }
    errors = @($text | Select-String 'WebGPU error|fatal|FATAL|rejected').Count
}
$json = $report | ConvertTo-Json
Set-Content -LiteralPath (Join-Path $runDir 'report.json') -Value $json -Encoding UTF8
$json
