param([switch]$NoHangCapture, [switch]$NoPerformanceTrace, [switch]$SyncRenderer)
$ErrorActionPreference = 'Stop'
$primeProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$primeBuildDirectory = Join-Path $primeProjectRoot 'recomp/build/RM3E01_00_runtime_link_probe'
$primeExecutable = Join-Path $primeBuildDirectory 'Corruption_3 PC.exe'
if (-not (Test-Path -LiteralPath $primeExecutable -PathType Leaf)) { throw 'Build the Prime runtime before launching it.' }
$primePreviousMode = [Environment]::GetEnvironmentVariable('WII_COMPILED_PRIME_KEYBOARD_MOUSE','Process')
$primePreviousPerformance = [Environment]::GetEnvironmentVariable('WII_COMPILED_PRIME_PERFORMANCE_TRACE','Process')
# Renderer speed options come from Config.toml [prime] (F1 > Rendimiento), on by default.
# -SyncRenderer forces the original single-thread renderer for A/B checks.
$primeSpeedVars = @('WII_COMPILED_PRIME_GPU_THREAD','WII_COMPILED_AURORA_OVERLAP_ENCODE','WII_COMPILED_AURORA_LAZY_END_DRAIN','WII_COMPILED_PRIME_PRESENT_ON_COPY')
$primeSpeedPrevious = @{}
foreach ($name in $primeSpeedVars) { $primeSpeedPrevious[$name] = [Environment]::GetEnvironmentVariable($name,'Process') }
Push-Location -LiteralPath $primeBuildDirectory
$primeWatcher=$null
try {
    [Environment]::SetEnvironmentVariable('WII_COMPILED_PRIME_KEYBOARD_MOUSE','1','Process')
    foreach ($name in $primeSpeedVars) { [Environment]::SetEnvironmentVariable($name,$(if($SyncRenderer){'0'}else{$null}),'Process') }
    [Environment]::SetEnvironmentVariable('WII_COMPILED_PRIME_PERFORMANCE_TRACE',$(if($NoPerformanceTrace){$null}else{'1'}),'Process')
    $primeGame=Start-Process -FilePath $primeExecutable -WorkingDirectory $primeBuildDirectory -NoNewWindow -PassThru
    if(-not $NoHangCapture) {
        $primeHash=(Get-FileHash -LiteralPath $primeExecutable -Algorithm SHA256).Hash.ToLowerInvariant()
        $primeWatchScript=Join-Path $PSScriptRoot 'watch_prime_hang.ps1'
        $primeWatchLog=Join-Path $primeProjectRoot ("recomp/.tools/manual_hang_watch_pid{0}_{1}.log" -f $primeGame.Id,[DateTime]::UtcNow.Ticks)
        $primeWatcher=Start-Process -FilePath powershell.exe -WindowStyle Hidden -PassThru -ArgumentList @(
            '-NoProfile','-ExecutionPolicy','Bypass','-File',('"'+$primeWatchScript+'"'),
            '-TargetProcessId',$primeGame.Id,'-ExpectedExecutableSha256',$primeHash
        ) -RedirectStandardOutput $primeWatchLog -RedirectStandardError ($primeWatchLog+'.err')
    }
    $primeGame.WaitForExit()
    if ($primeGame.ExitCode -ne 0) { throw "Prime runtime exited with code $($primeGame.ExitCode)" }
} finally {
    if($primeWatcher -and -not $primeWatcher.HasExited){$primeWatcher.WaitForExit(2000) | Out-Null}
    [Environment]::SetEnvironmentVariable('WII_COMPILED_PRIME_KEYBOARD_MOUSE',$primePreviousMode,'Process')
    [Environment]::SetEnvironmentVariable('WII_COMPILED_PRIME_PERFORMANCE_TRACE',$primePreviousPerformance,'Process')
    foreach ($name in $primeSpeedVars) { [Environment]::SetEnvironmentVariable($name,$primeSpeedPrevious[$name],'Process') }
    Pop-Location
}
