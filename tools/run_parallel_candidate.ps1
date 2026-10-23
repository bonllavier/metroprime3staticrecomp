param([switch]$PerformanceTrace)
$ErrorActionPreference = 'Stop'
$scriptRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$buildDirectory = Join-Path $scriptRoot 'recomp/build/RM3E01_00_runtime_link_probe'
$executable = Join-Path $buildDirectory 'WiiCompiled.exe'
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
    throw "Parallel candidate not found: $executable"
}
$running = @(Get-Process -Name 'WiiCompiled','WiiCompiled_parallel_candidate' -ErrorAction SilentlyContinue)
if ($running.Count -gt 0) {
    $ids = ($running | ForEach-Object { $_.Id }) -join ', '
    throw "Close the existing WiiCompiled instance(s) before starting the candidate (PID $ids)."
}
$previousMode = [Environment]::GetEnvironmentVariable('WII_COMPILED_PRIME_KEYBOARD_MOUSE','Process')
$previousTrace = [Environment]::GetEnvironmentVariable('WII_COMPILED_PRIME_PERFORMANCE_TRACE','Process')
try {
    [Environment]::SetEnvironmentVariable('WII_COMPILED_PRIME_KEYBOARD_MOUSE','1','Process')
    if ($PerformanceTrace) { [Environment]::SetEnvironmentVariable('WII_COMPILED_PRIME_PERFORMANCE_TRACE','1','Process') }
    $process = Start-Process -FilePath $executable -WorkingDirectory $buildDirectory -PassThru
} finally {
    [Environment]::SetEnvironmentVariable('WII_COMPILED_PRIME_KEYBOARD_MOUSE',$previousMode,'Process')
    [Environment]::SetEnvironmentVariable('WII_COMPILED_PRIME_PERFORMANCE_TRACE',$previousTrace,'Process')
}
Start-Sleep -Milliseconds 500
$process.Refresh()
if ($process.HasExited) {
    throw "Parallel candidate exited during startup with code $($process.ExitCode). Check UserData/Logs."
}
Write-Output "Started keyboard/mouse-enabled candidate: PID $($process.Id)"
if ($PerformanceTrace) { Write-Output 'GX performance and raw texture cache trace enabled.' }
Write-Output $process.Path
