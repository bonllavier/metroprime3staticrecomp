param(
    [Parameter(Mandatory=$true)][int]$TargetProcessId,
    [Parameter(Mandatory=$true)][string]$ExpectedExecutableSha256,
    [ValidateRange(5,120)][int]$UnresponsiveSeconds=15,
    [ValidateRange(10,43200)][int]$MaximumSeconds=14400
)
$ErrorActionPreference='Stop'
$taskRoot=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$taskProcess=Get-Process -Id $TargetProcessId -ErrorAction SilentlyContinue
if(-not $taskProcess){exit 0}
$taskExe=$taskProcess.MainModule.FileName
if([IO.Path]::GetFileName($taskExe) -ne 'Corruption_3 PC.exe'){throw 'Unexpected target'}
$taskHash=(Get-FileHash -LiteralPath $taskExe -Algorithm SHA256).Hash.ToLowerInvariant()
if($taskHash -ne $ExpectedExecutableSha256.ToLowerInvariant()){throw 'Executable hash differs'}
$taskStarted=$taskProcess.StartTime.ToUniversalTime()
$taskTimer=[Diagnostics.Stopwatch]::StartNew()
$taskUnresponsive=$null
while($taskTimer.Elapsed.TotalSeconds -lt $MaximumSeconds) {
    $taskProcess.Refresh()
    if($taskProcess.HasExited){exit 0}
    if($taskProcess.StartTime.ToUniversalTime() -ne $taskStarted){throw 'PID reused'}
    if($taskProcess.MainWindowHandle -ne [IntPtr]::Zero -and -not $taskProcess.Responding) {
        if($null -eq $taskUnresponsive){$taskUnresponsive=$taskTimer.Elapsed.TotalSeconds}
        if($taskTimer.Elapsed.TotalSeconds-$taskUnresponsive -ge $UnresponsiveSeconds) {
            $taskOutput=Join-Path $taskRoot ('recomp/.tools/manual_hang_pid{0}_{1}.json' -f $TargetProcessId,[DateTime]::UtcNow.ToString('yyyyMMdd_HHmmss_ffff'))
            & python (Join-Path $PSScriptRoot 'capture_prime_hang.py') --pid $TargetProcessId --output $taskOutput
            if($LASTEXITCODE -ne 0){throw 'Live hang capture failed'}
            Write-Output "Native hang snapshot: $taskOutput"
            exit 0
        }
    } else {$taskUnresponsive=$null}
    Start-Sleep -Milliseconds 500
}
# Passive bounded observer: never inject input, terminate or restart the game.
