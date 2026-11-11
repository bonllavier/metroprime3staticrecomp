param(
    [ValidateRange(1,7200)][int]$Seconds = 15,
    [switch]$CaptureWindow,
    [ValidateRange(256,16384)][int]$MinimumFreeMegabytes = 1024,
    [int[]]$CaptureAtSeconds = @(15, 40),
    [string]$BuildDirectory = 'recomp/build/RM3E01_00_runtime_link_probe'
)
$ErrorActionPreference = 'Stop'
$taskRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$taskBuild = Join-Path $taskRoot $BuildDirectory
$taskExe = Join-Path $taskBuild 'Corruption_3 PC.exe'
$taskDrive = [IO.DriveInfo]::new([IO.Path]::GetPathRoot($taskBuild))
$taskFreeBytesAtStart = $taskDrive.AvailableFreeSpace
if ($taskFreeBytesAtStart -lt ($MinimumFreeMegabytes * 1MB)) {
    throw "Insufficient free space for probe evidence: $($taskDrive.Name) has $taskFreeBytesAtStart bytes; requires $MinimumFreeMegabytes MiB. No game process started."
}
$taskHash = (Get-FileHash -LiteralPath $taskExe -Algorithm SHA256).Hash.ToLowerInvariant()
$taskKnownLogDirectories = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($taskDirectory in Get-ChildItem -LiteralPath (Join-Path $taskBuild 'UserData/Logs') -Directory) {
    [void]$taskKnownLogDirectories.Add($taskDirectory.Name)
}
$taskProbeScriptHash = (Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash.ToLowerInvariant()
$taskStart = [DateTime]::UtcNow
$taskPreviousProbe = [Environment]::GetEnvironmentVariable('WII_COMPILED_AUTOMATED_PROBE', 'Process')
$taskPreviousKeyboard = [Environment]::GetEnvironmentVariable('WII_COMPILED_PRIME_KEYBOARD_MOUSE', 'Process')
$taskPreviousGpuValidation = [Environment]::GetEnvironmentVariable('WII_COMPILED_VALIDATE_GPU', 'Process')
try {
    [Environment]::SetEnvironmentVariable('WII_COMPILED_AUTOMATED_PROBE', '1', 'Process')
    [Environment]::SetEnvironmentVariable('WII_COMPILED_VALIDATE_GPU', '1', 'Process')
    $taskProcess = Start-Process -FilePath $taskExe -PassThru -WindowStyle Hidden
} finally {
    [Environment]::SetEnvironmentVariable('WII_COMPILED_PRIME_KEYBOARD_MOUSE', $taskPreviousKeyboard, 'Process')
    [Environment]::SetEnvironmentVariable('WII_COMPILED_AUTOMATED_PROBE', $taskPreviousProbe, 'Process')
    [Environment]::SetEnvironmentVariable('WII_COMPILED_VALIDATE_GPU', $taskPreviousGpuValidation, 'Process')
}
$taskAlive = $false
$taskCaptureReports = @()
$taskCaptureErrors = @()
$taskWatch = [Diagnostics.Stopwatch]::StartNew()
$taskProgressPath=Join-Path $taskBuild ("UserData/Logs/probe_pid{0}_{1}.progress.json" -f $taskProcess.Id,$taskStart.ToString('yyyyMMdd_HHmmss_ffff'))
function Save-PrimeProbeProgress([string]$stage) {
    $taskProgress=[ordered]@{observation_complete=$false;stage=$stage;pid=$taskProcess.Id;started_utc=$taskStart.ToString('o');executable_sha256=$taskHash;probe_script_sha256=$taskProbeScriptHash;observation_seconds=$Seconds;captures=$taskCaptureReports}
    Save-PrimeProbeJson $taskProgressPath ($taskProgress|ConvertTo-Json -Depth 5)
}
function Save-PrimeProbeJson([string]$path, [string]$json) {
    $temporary = $path + '.tmp'
    [IO.File]::WriteAllText($temporary, $json + [Environment]::NewLine, [Text.UTF8Encoding]::new($false))
    if ([IO.File]::Exists($path)) { [IO.File]::Replace($temporary, $path, [NullString]::Value) }
    else { [IO.File]::Move($temporary, $path) }
}
Save-PrimeProbeProgress 'started'
try {
    if ($CaptureWindow) {
        foreach ($taskCaptureSecond in ($CaptureAtSeconds | Sort-Object -Unique)) {
            if ($taskCaptureSecond -le 0 -or $taskCaptureSecond -ge $Seconds) { continue }
            $taskRemainingMs = [Math]::Max(0, [int]($taskCaptureSecond * 1000 - $taskWatch.ElapsedMilliseconds))
            if ($taskProcess.WaitForExit($taskRemainingMs)) { break }
            $taskImage = Join-Path $taskBuild "UserData/Logs/window_pid$($taskProcess.Id)_$($taskCaptureSecond)s.png"
            try {
                $taskCapture = (& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'capture_prime_window.ps1') -TargetProcessId $taskProcess.Id -OutputPath $taskImage | Out-String | ConvertFrom-Json)
                if ($null -eq $taskCapture.print_window_return -or -not (Test-Path -LiteralPath $taskImage) -or (Get-Item -LiteralPath $taskImage).Length -eq 0) {
                    throw 'Capture did not produce a nonempty image and a capture result'
                }
                $taskCaptureReports += [ordered]@{ at_second=$taskCaptureSecond; path=$taskImage; print_window_return=$taskCapture.print_window_return; sha256=(Get-FileHash -LiteralPath $taskImage -Algorithm SHA256).Hash.ToLowerInvariant() }
                Save-PrimeProbeProgress 'captured'
            } catch {
                $taskCaptureErrors += [ordered]@{ at_second=$taskCaptureSecond; error=$_.ToString() }
                Write-Output "Window capture failed: $_"
            }
        }
    }
    $taskRemainingMs = [Math]::Max(0, [int]($Seconds * 1000 - $taskWatch.ElapsedMilliseconds))
    $taskFinished = $taskProcess.WaitForExit($taskRemainingMs)
    $taskAlive = -not $taskFinished
} finally {
    if (-not $taskProcess.HasExited) { Stop-Process -Id $taskProcess.Id -Force }
    $taskProcess.WaitForExit()
}
$taskRun = Get-ChildItem -LiteralPath (Join-Path $taskBuild 'UserData\Logs') -Directory |
    Where-Object { $_.Name -like "*pid$($taskProcess.Id)" -and -not $taskKnownLogDirectories.Contains($_.Name) }
if (@($taskRun).Count -ne 1) { throw "Expected exactly one new run log directory for PID $($taskProcess.Id)" }
if ($null -eq $taskRun) { throw "No runtime log directory for PID $($taskProcess.Id)" }
$taskLog = Join-Path $taskRun.FullName 'console.log'
$taskText = Get-Content -LiteralPath $taskLog -Raw
$taskReport = [ordered]@{
    revision = 'RM3E01_00'
    started_utc = $taskStart.ToString('o')
    pid = $taskProcess.Id
    observation_seconds = $Seconds
    alive_at_deadline = $taskAlive
    stopped_by_harness = $taskAlive
    process_exit_code = $taskProcess.ExitCode
    fatal_dialogs_suppressed = $true
    gpu_validation_requested = $true
    executable_sha256 = $taskHash
    probe_script_sha256 = $taskProbeScriptHash
    log = $taskLog
    log_sha256 = (Get-FileHash -LiteralPath $taskLog -Algorithm SHA256).Hash.ToLowerInvariant()
    failure_logged = [bool]($taskText -match '(?i)WebGPU error|Unhandled C\+\+ exception:|std::terminate\(\) called|Runtime error:|\[fatal\]|FATAL unmapped guest address|memory error at|(?<!no )unmapped guest touch|Unsupported IOS IPC submit|OH1 ioctlv unsupported|not translated \(missing function\)|Missing translated function|Missing indirect jump target|InvokeIndirectJump: target .*not a registered function|\[probe-fatal-dialog-suppressed\]')
    captures = $taskCaptureReports
    capture_errors = $taskCaptureErrors
    storage_free_bytes_at_start = $taskFreeBytesAtStart
    storage_minimum_free_megabytes = $MinimumFreeMegabytes
    first_game_screen_verified = $false
    note = 'Being alive and creating a graphics window do not prove game execution or a rendered screen.'
}
$taskJson = $taskReport | ConvertTo-Json -Depth 4
$taskOut = Join-Path $taskRun.FullName 'runtime_probe.json'
Save-PrimeProbeJson $taskOut $taskJson
Save-PrimeProbeJson $taskProgressPath ([ordered]@{observation_complete=$true;pid=$taskProcess.Id;final_report=$taskOut}|ConvertTo-Json)
$taskJson
Get-Content -LiteralPath $taskLog -Tail 30 | Where-Object {$_ -notmatch '\x00'} | Select-Object -Last 15
