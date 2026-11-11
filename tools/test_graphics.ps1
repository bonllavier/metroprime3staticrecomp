<#
 Runs every automated check of the graphics/input layer and writes a checkpoint.
   Aurora   : gx_fifo_tests (GX FIFO, renderer regressions, shader golden), blob_cache_policy_tests, render_policy_tests
   Runtime  : ctest of runtime-platform-tests (config, mouse look, lasso, BP/FIFO captures...)
   Python   : prime_icons/test_icon_table.py
 Needs no ISO and no GPU. Rebuilds only what changed. Exit code 1 if anything fails.
 Intended shader generator change:  $env:UPDATE_GOLDEN=1; gx_fifo_tests --gtest_filter='ShaderGolden.*'  (see docs/pruebas_graficas.md)
#>
param([switch]$SkipBuild)
$ErrorActionPreference = 'Stop'
$recomp = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$tools = Join-Path $recomp '.tools/portable-tools'
$env:PATH = (Join-Path $tools 'llvm-mingw/bin') + ';' + (Join-Path $tools 'Ninja') + ';' + $env:PATH
$auroraDir = Join-Path $recomp 'build/aurora-rm3-dawn-20261016-probe'
$platformDir = Join-Path $recomp 'build/runtime-platform-tests'
$results = @(); $failed = $false

function Step($name, [scriptblock]$body) {
    Write-Host "== $name"
    $ErrorActionPreference = 'Continue'
    $out = & $body 2>&1 | ForEach-Object { "$_" } | Out-String
    $code = $LASTEXITCODE
    $summary = ($out -split "`r?`n" | Where-Object { $_ -match '\[  PASSED  \]|\[  FAILED  \]|tests passed|tests failed|^PASS|OK$|Ran \d+ test|FAILED' } | Select-Object -First 4) -join ' | '
    if ($code -ne 0) { $script:failed = $true; Write-Host $out }
    Write-Host ("   exit {0}  {1}" -f $code, $summary)
    $script:results += [ordered]@{ name = $name; exit_code = $code; summary = $summary }
}

if (-not $SkipBuild) {
    Step 'build aurora tests' { cmake --build $auroraDir --target gx_fifo_tests blob_cache_policy_tests render_policy_tests --parallel 2 }
    Step 'build runtime tests' { cmake --build $platformDir --parallel 2 }
}
foreach ($t in 'gx_fifo_tests', 'blob_cache_policy_tests', 'render_policy_tests') {
    Step "aurora/$t" { & (Join-Path $auroraDir "tests/$t.exe") }
}
Step 'runtime ctest' { ctest --test-dir $platformDir --output-on-failure }
Step 'python icon table' { python -I (Join-Path $recomp 'tools/prime_icons/test_icon_table.py') }
Step 'python seed filter' { python -I (Join-Path $recomp 'tools/test_prime_pipeline_seed.py') }

$report = [ordered]@{
    tested_utc = [DateTime]::UtcNow.ToString('o')
    revision = 'RM3E01_00'
    passed = -not $failed
    steps = $results
    limitation = 'No GPU or ISO: covers pure policy, generator output (golden), cache SQL, config parsing and input logic. Rendering correctness on screen stays a manual check.'
}
$json = $report | ConvertTo-Json -Depth 5
[System.IO.File]::WriteAllText((Join-Path $recomp 'checkpoints/20261026_graphics_test_suite.json'), $json + [Environment]::NewLine, [System.Text.UTF8Encoding]::new($false))
if ($failed) { Write-Host 'FAILED'; exit 1 } else { Write-Host 'ALL PASSED' }
