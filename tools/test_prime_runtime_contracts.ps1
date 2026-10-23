param([string]$BuildDirectory = 'recomp/build/prime_contracts')
$ErrorActionPreference = 'Stop'
$taskRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$taskOutput = Join-Path $taskRoot $BuildDirectory
$taskCompiler = Join-Path $taskRoot 'recomp/.tools/portable-tools/llvm-mingw/bin/clang++.exe'
$taskInclude = Join-Path $taskRoot 'recomp/third_party/WiiCompiled/runtime/include'
$taskHostSource = Join-Path $taskRoot 'recomp/third_party/WiiCompiled/runtime/src/host_context.cpp'
New-Item -ItemType Directory -Path $taskOutput -Force | Out-Null
$taskResults = @()
$taskPreviousPath = $env:PATH
try {
    $env:PATH = (Split-Path -Parent $taskCompiler) + ';' + $env:PATH
    foreach ($taskName in @('prime_ai_registers', 'prime_dsp_boot_registers', 'prime_host_continuations', 'prime_rso_relocations')) {
        $taskSource = Join-Path $PSScriptRoot ($taskName + '_tests.cpp')
        $taskBinary = Join-Path $taskOutput ($taskName + '_tests.exe')
        $taskArguments = @('-std=c++17', '-O2', '-UNDEBUG', '-I', $taskInclude, $taskSource)
        if ($taskName -eq 'prime_host_continuations') { $taskArguments += $taskHostSource }
        $taskArguments += @('-o', $taskBinary)
        & $taskCompiler @taskArguments
        if ($LASTEXITCODE -ne 0) { throw "Compilation failed: $taskName ($LASTEXITCODE)" }
        $taskText = (& $taskBinary 2>&1 | Out-String).Trim()
        if ($LASTEXITCODE -ne 0) { throw "Contract failed: $taskName ($LASTEXITCODE): $taskText" }
        if ($taskText -notlike 'PASS *') { throw "Missing PASS evidence: $taskName" }
        $taskResults += [ordered]@{
            name = $taskName
            exit_code = $LASTEXITCODE
            output = $taskText
            executable_sha256 = (Get-FileHash -LiteralPath $taskBinary -Algorithm SHA256).Hash.ToLowerInvariant()
            source_sha256 = (Get-FileHash -LiteralPath $taskSource -Algorithm SHA256).Hash.ToLowerInvariant()
            header_sha256 = (Get-FileHash -LiteralPath (Join-Path $taskInclude ('hle/' + $taskName + '.h')) -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    }
} finally { $env:PATH = $taskPreviousPath }
$taskReport = [ordered]@{
    tested_utc = [DateTime]::UtcNow.ToString('o')
    revision = 'RM3E01_00'
    compiler_sha256 = (Get-FileHash -LiteralPath $taskCompiler -Algorithm SHA256).Hash.ToLowerInvariant()
    assertions_enabled = $true
    tests = $taskResults
    limitation = 'Component contracts only. Does not establish game screen, full SDK thread behavior, DSP mix fidelity or audio output.'
}
$taskJson = $taskReport | ConvertTo-Json -Depth 6
$taskCheckpoint = Join-Path $taskRoot 'recomp/checkpoints/20261020_prime_runtime_contract_tests.json'
[System.IO.File]::WriteAllText($taskCheckpoint, $taskJson + [Environment]::NewLine, [System.Text.UTF8Encoding]::new($false))
$taskJson
