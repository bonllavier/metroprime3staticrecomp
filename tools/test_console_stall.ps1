# Regression check for the 2026-10-26 freeze (docs/congelacion_consola.md): the game is started with its
# console output going to pipes that nobody reads, which is what a PowerShell window in "Select" mode
# amounts to. Before the fix the game froze within seconds; now it must keep running and keep logging.
#   test_console_stall.ps1 [-Seconds 45]
param([int]$Seconds = 45)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$dir = Join-Path $root 'recomp/build/RM3E01_00_runtime_link_probe'
if (@(Get-Process | Where-Object { $_.ProcessName -like 'Corruption_3*' }).Count) { throw 'Close the game first.' }
$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = Join-Path $dir 'Corruption_3 PC.exe'
$psi.WorkingDirectory = $dir
$psi.UseShellExecute = $false
$psi.RedirectStandardOutput = $true   # never read: the pipe fills after 4 KB, like a stalled console
$psi.RedirectStandardError = $true
$psi.EnvironmentVariables['WII_COMPILED_QA_TAP_INTRO'] = '1'
$p = [System.Diagnostics.Process]::Start($psi)
try {
    Start-Sleep -Seconds $Seconds
    $p.Refresh()
    $log = Get-ChildItem (Join-Path $dir 'UserData/Logs') -Directory | Where-Object { $_.Name -like "*pid$($p.Id)" } | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    $console = Join-Path $log.FullName 'console.log'
    $size1 = (Get-Item $console).Length
    Start-Sleep -Seconds 6
    $size2 = (Get-Item $console).Length
    $responding = -not $p.HasExited -and $p.Responding
    $dropped = (Select-String -Path $console -Pattern 'transcript: dropped').Count
    [pscustomobject]@{ pid = $p.Id; responding = $responding; log_grew_bytes = $size2 - $size1; console_drop_reports = $dropped; passed = ($responding -and ($size2 -gt $size1)) } | Format-List
} finally {
    if (-not $p.HasExited) { $p.Kill() }
}
