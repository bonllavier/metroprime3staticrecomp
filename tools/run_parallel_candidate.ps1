$ErrorActionPreference = 'Stop'
$scriptRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$buildDirectory = Join-Path $scriptRoot 'recomp/build/RM3E01_00_runtime_link_probe'
$executable = Join-Path $buildDirectory 'WiiCompiled_parallel_candidate.exe'
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
    throw "Parallel candidate not found: $executable"
}
$running = @(Get-Process -Name 'WiiCompiled','WiiCompiled_parallel_candidate' -ErrorAction SilentlyContinue)
if ($running.Count -gt 0) {
    $ids = ($running | ForEach-Object { $_.Id }) -join ', '
    throw "Close the existing WiiCompiled instance(s) before starting the candidate (PID $ids)."
}
$previousMode = [Environment]::GetEnvironmentVariable('WII_COMPILED_PRIME_KEYBOARD_MOUSE','Process')
try {
    [Environment]::SetEnvironmentVariable('WII_COMPILED_PRIME_KEYBOARD_MOUSE','1','Process')
    $process = Start-Process -FilePath $executable -WorkingDirectory $buildDirectory -PassThru
} finally {
    [Environment]::SetEnvironmentVariable('WII_COMPILED_PRIME_KEYBOARD_MOUSE',$previousMode,'Process')
}
Start-Sleep -Milliseconds 500
$process.Refresh()
if ($process.HasExited) {
    throw "Parallel candidate exited during startup with code $($process.ExitCode). Check UserData/Logs."
}
Write-Output "Started keyboard/mouse-enabled candidate: PID $($process.Id)"
Write-Output $process.Path
