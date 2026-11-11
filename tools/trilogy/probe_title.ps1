# Runs a Trilogy executable on a private profile for a while, with screenshots and the logs.
# Follows OSExec into the next executable. -Script is a replay of keyframes in qa/ input format.
#   probe_title.ps1 -Exe "Corruption_3 Trilogy Menu.exe" -Seconds 60 -Shots 10,30,55 -Captures 40 -Label first
param([string]$Exe = 'Corruption_3 Trilogy Menu.exe', [int]$Seconds = 60, [string]$Shots = '15,40', [string]$Label = 'run', [string]$Env = '', [string]$Captures = '', [string]$Script = '', [switch]$KeepProfile)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$build = Join-Path $root 'build\RM3E01_00_runtime_link_probe'
$profile = Join-Path $root 'titles\probe_profile'
$image = 'C:\Users\MENTO\Downloads\isos_tmp\Metroid Prime Trilogy (USA).rvz'
function GameProcesses { @(Get-Process | Where-Object { $_.ProcessName -like 'Corruption_3*' }) }
if ((GameProcesses).Count) { throw 'A game instance is already running.' }
if (-not (Test-Path $profile)) { New-Item -ItemType Directory -Path $profile | Out-Null }
if (-not $KeepProfile -or -not (Test-Path (Join-Path $profile 'Config.toml'))) {
    $config = [IO.File]::ReadAllText((Join-Path $root 'qa\recordings\hangar_1x_window\Config.toml'))
    $config = [regex]::Replace($config, '(?m)^iso_path = .*$', ('iso_path = "' + $image.Replace('\', '/') + '"'))
    [IO.File]::WriteAllText((Join-Path $profile 'Config.toml'), $config)
}
$runDir = Join-Path $root "titles\probe_runs\$((Get-Date).ToString('yyyyMMdd_HHmmss'))_$Label"
New-Item -ItemType Directory -Path $runDir | Out-Null
$vars = @{ WII_COMPILED_QA_USER_DATA = $profile; WII_COMPILED_PRIME_KEYBOARD_MOUSE = '1'; WII_COMPILED_PRIME_PERFORMANCE_TRACE = '1' }
if ($Script) { $vars['WII_COMPILED_QA_REPLAY'] = (Resolve-Path $Script).Path }
foreach ($pair in ($Env -split ';' | Where-Object { $_ })) { $k, $v = $pair -split '=', 2; $vars[$k] = $v }
foreach ($k in $vars.Keys) { [Environment]::SetEnvironmentVariable($k, $vars[$k], 'Process') }
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class ProbeWin { [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern bool IsHungAppWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags); }
"@
function Shot($proc, $path) {
    try {
        $proc.Refresh()
        if ($proc.MainWindowHandle -eq [IntPtr]::Zero) { return }
        # A window that stopped pumping messages would block ShowWindow/SetWindowPos for good.
        if ([ProbeWin]::IsHungAppWindow($proc.MainWindowHandle)) { "hung window at $path" | Add-Content (Join-Path $runDir 'notes.txt'); return }
        # Topmost only for the shot and without taking focus: the user may be working on this machine.
        [void][ProbeWin]::ShowWindow($proc.MainWindowHandle, 4)
        [void][ProbeWin]::SetWindowPos($proc.MainWindowHandle, [IntPtr](-1), 0, 0, 0, 0, 0x53)
        Start-Sleep -Milliseconds 400
        $r = New-Object ProbeWin+RECT; [void][ProbeWin]::GetClientRect($proc.MainWindowHandle, [ref]$r)
        $pt = New-Object ProbeWin+POINT; [void][ProbeWin]::ClientToScreen($proc.MainWindowHandle, [ref]$pt)
        if ($r.R -le 0) { return }
        $bmp = New-Object System.Drawing.Bitmap $r.R, $r.B; $g = [System.Drawing.Graphics]::FromImage($bmp)
        $g.CopyFromScreen($pt.X, $pt.Y, 0, 0, $bmp.Size); $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png); $g.Dispose(); $bmp.Dispose()
        [void][ProbeWin]::SetWindowPos($proc.MainWindowHandle, [IntPtr](-2), 0, 0, 0, 0, 0x53)
    } catch {}
}
$p = Start-Process -FilePath (Join-Path $build $Exe) -WorkingDirectory $build -PassThru
$start = Get-Date
$pids = [Collections.Generic.List[int]]::new(); $pids.Add($p.Id)
$pending = [Collections.Generic.List[int]]::new(); $Shots -split ',' | ForEach-Object { $pending.Add([int]$_) }
$capturePending = [Collections.Generic.List[int]]::new(); $Captures -split ',' | Where-Object { $_ } | ForEach-Object { $capturePending.Add([int]$_) }
while (((Get-Date) - $start).TotalSeconds -lt $Seconds) {
    Start-Sleep -Milliseconds 500
    if ($p.HasExited) {
        # OSExec started the next executable, or the run is over.
        $next = GameProcesses | Select-Object -First 1
        if (-not $next) { Start-Sleep 2; $next = GameProcesses | Select-Object -First 1 }
        if (-not $next) { break }
        $p = $next
        if (-not $pids.Contains($p.Id)) { $pids.Add($p.Id); "$([int]((Get-Date) - $start).TotalSeconds)s -> pid $($p.Id) $($p.ProcessName)" | Add-Content (Join-Path $runDir 'notes.txt') }
    }
    $elapsed = ((Get-Date) - $start).TotalSeconds
    if ($pending.Count -and $elapsed -ge $pending[0]) { Shot $p (Join-Path $runDir ("shot_{0:D4}s.png" -f $pending[0])); $pending.RemoveAt(0) }
    if ($capturePending.Count -and $elapsed -ge $capturePending[0]) {
        & 'G:/gamecube_roms/metroid3_wii/.venv/Scripts/python.exe' -B (Join-Path $root 'tools\capture_prime_hang.py') --pid $p.Id --output (Join-Path $runDir ("hang_{0:D4}s.json" -f $capturePending[0])) | Out-Null
        $capturePending.RemoveAt(0)
    }
}
$remaining = GameProcesses
$remaining | ForEach-Object { Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue }
Start-Sleep 2
foreach ($id in $pids) {
    $log = Get-ChildItem (Join-Path $profile 'Logs') -Directory | Where-Object { $_.Name -like "*pid$id" } | Select-Object -First 1
    if ($log) { Copy-Item (Join-Path $log.FullName 'console.log') (Join-Path $runDir "console_$id.log") }
}
"pids=$($pids -join ',') still_running_at_end=$($remaining.Count) seconds=$([int]((Get-Date) - $start).TotalSeconds) run=$runDir"
